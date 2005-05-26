/*-------------------------------------------------------------------------
 *
 * date.c
 *	  implements DATE and TIME data types specified in SQL-92 standard
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/date.c,v 1.93.2.3 2005/05/26 02:14:31 neilc Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>
#include <limits.h>
#include <time.h>
#include <float.h>

#include "access/hash.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/nabstime.h"
#include "utils/timestamp.h"

/*
 * gcc's -ffast-math switch breaks routines that expect exact results from
 * expressions like timeval / 3600, where timeval is double.
 */
#ifdef __FAST_MATH__
#error -ffast-math is known to break this code
#endif


static int	time2tm(TimeADT time, struct tm * tm, fsec_t *fsec);
static int	timetz2tm(TimeTzADT *time, struct tm * tm, fsec_t *fsec, int *tzp);
static int	tm2time(struct tm * tm, fsec_t fsec, TimeADT *result);
static int	tm2timetz(struct tm * tm, fsec_t fsec, int tz, TimeTzADT *result);
static void AdjustTimeForTypmod(TimeADT *time, int32 typmod);

/*****************************************************************************
 *	 Date ADT
 *****************************************************************************/


/* date_in()
 * Given date text string, convert to internal date format.
 */
Datum
date_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	DateADT		date;
	fsec_t		fsec;
	struct tm	tt,
			   *tm = &tt;
	int			tzp;
	int			dtype;
	int			nf;
	int			dterr;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		workbuf[MAXDATELEN + 1];

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf),
						  field, ftype, MAXDATEFIELDS, &nf);
	if (dterr == 0)
		dterr = DecodeDateTime(field, ftype, nf, &dtype, tm, &fsec, &tzp);
	if (dterr != 0)
		DateTimeParseError(dterr, str, "date");

	switch (dtype)
	{
		case DTK_DATE:
			break;

		case DTK_CURRENT:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("date/time value \"current\" is no longer supported")));

			GetCurrentDateTime(tm);
			break;

		case DTK_EPOCH:
			GetEpochTime(tm);
			break;

		default:
			DateTimeParseError(DTERR_BAD_FORMAT, str, "date");
			break;
	}

	date = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - POSTGRES_EPOCH_JDATE;

	PG_RETURN_DATEADT(date);
}

/* date_out()
 * Given internal format date, convert to text string.
 */
Datum
date_out(PG_FUNCTION_ARGS)
{
	DateADT		date = PG_GETARG_DATEADT(0);
	char	   *result;
	struct tm	tt,
			   *tm = &tt;
	char		buf[MAXDATELEN + 1];

	j2date(date + POSTGRES_EPOCH_JDATE,
		   &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));

	EncodeDateOnly(tm, DateStyle, buf);

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

/*
 *		date_recv			- converts external binary format to date
 */
Datum
date_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_DATEADT((DateADT) pq_getmsgint(buf, sizeof(DateADT)));
}

/*
 *		date_send			- converts date to binary format
 */
Datum
date_send(PG_FUNCTION_ARGS)
{
	DateADT		date = PG_GETARG_DATEADT(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint(&buf, date, sizeof(date));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


Datum
date_eq(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(dateVal1 == dateVal2);
}

Datum
date_ne(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(dateVal1 != dateVal2);
}

Datum
date_lt(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(dateVal1 < dateVal2);
}

Datum
date_le(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(dateVal1 <= dateVal2);
}

Datum
date_gt(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(dateVal1 > dateVal2);
}

Datum
date_ge(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_BOOL(dateVal1 >= dateVal2);
}

Datum
date_cmp(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	if (dateVal1 < dateVal2)
		PG_RETURN_INT32(-1);
	else if (dateVal1 > dateVal2)
		PG_RETURN_INT32(1);
	PG_RETURN_INT32(0);
}

Datum
date_larger(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_DATEADT((dateVal1 > dateVal2) ? dateVal1 : dateVal2);
}

Datum
date_smaller(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_DATEADT((dateVal1 < dateVal2) ? dateVal1 : dateVal2);
}

/* Compute difference between two dates in days.
 */
Datum
date_mi(PG_FUNCTION_ARGS)
{
	DateADT		dateVal1 = PG_GETARG_DATEADT(0);
	DateADT		dateVal2 = PG_GETARG_DATEADT(1);

	PG_RETURN_INT32((int32) (dateVal1 - dateVal2));
}

/* Add a number of days to a date, giving a new date.
 * Must handle both positive and negative numbers of days.
 */
Datum
date_pli(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	int32		days = PG_GETARG_INT32(1);

	PG_RETURN_DATEADT(dateVal + days);
}

/* Subtract a number of days from a date, giving a new date.
 */
Datum
date_mii(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	int32		days = PG_GETARG_INT32(1);

	PG_RETURN_DATEADT(dateVal - days);
}

#if NOT_USED
/* date_pl_interval() and date_mi_interval() are probably
 * better implmented by converting the input date
 * to timestamp without time zone. So that is what we do
 * in pg_proc.h - thomas 2002-03-11
 */

/* Add an interval to a date, giving a new date.
 * Must handle both positive and negative intervals.
 */
Datum
date_pl_interval(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	struct tm	tt,
			   *tm = &tt;

	if (span->month != 0)
	{
		j2date(dateVal + POSTGRES_EPOCH_JDATE,
			   &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));
		tm->tm_mon += span->month;
		dateVal = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - POSTGRES_EPOCH_JDATE;
	}
	if (span->time != 0)
		dateVal += (span->time / 86400e0);

	PG_RETURN_DATEADT(dateVal);
}

/* Subtract an interval from a date, giving a new date.
 * Must handle both positive and negative intervals.
 */
Datum
date_mi_interval(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	struct tm	tt,
			   *tm = &tt;

	if (span->month != 0)
	{
		j2date(dateVal + POSTGRES_EPOCH_JDATE,
			   &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));
		tm->tm_mon -= span->month;
		dateVal = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - POSTGRES_EPOCH_JDATE;
	}
	if (span->time != 0)
		dateVal -= (span->time / 86400e0);

	PG_RETURN_DATEADT(dateVal);
}
#endif

/* date_timestamp()
 * Convert date to timestamp data type.
 */
Datum
date_timestamp(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	Timestamp	result;

#ifdef HAVE_INT64_TIMESTAMP
	/* date is days since 2000, timestamp is microseconds since same... */
	result = dateVal * INT64CONST(86400000000);
#else
	/* date is days since 2000, timestamp is seconds since same... */
	result = dateVal * 86400.0;
#endif

	PG_RETURN_TIMESTAMP(result);
}


/* timestamp_date()
 * Convert timestamp to date data type.
 */
Datum
timestamp_date(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	DateADT		result;
	struct tm	tt,
			   *tm = &tt;
	fsec_t		fsec;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_NULL();

	if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	result = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - POSTGRES_EPOCH_JDATE;

	PG_RETURN_DATEADT(result);
}


/* date_timestamptz()
 * Convert date to timestamp with time zone data type.
 */
Datum
date_timestamptz(PG_FUNCTION_ARGS)
{
	DateADT		dateVal = PG_GETARG_DATEADT(0);
	TimestampTz result;
	struct tm	tt,
			   *tm = &tt;

	j2date(dateVal + POSTGRES_EPOCH_JDATE,
		   &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));

	if (IS_VALID_UTIME(tm->tm_year, tm->tm_mon, tm->tm_mday))
	{
		int			tz;

		tm->tm_hour = 0;
		tm->tm_min = 0;
		tm->tm_sec = 0;
		tz = DetermineLocalTimeZone(tm);

#ifdef HAVE_INT64_TIMESTAMP
		result = (dateVal * INT64CONST(86400000000))
			+ (tz * INT64CONST(1000000));
#else
		result = dateVal * 86400.0 + tz;
#endif
	}
	else
	{
		/* Outside of range for timezone support, so assume UTC */
#ifdef HAVE_INT64_TIMESTAMP
		result = (dateVal * INT64CONST(86400000000));
#else
		result = dateVal * 86400.0;
#endif
	}

	PG_RETURN_TIMESTAMP(result);
}


/* timestamptz_date()
 * Convert timestamp with time zone to date data type.
 */
Datum
timestamptz_date(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMP(0);
	DateADT		result;
	struct tm	tt,
			   *tm = &tt;
	fsec_t		fsec;
	int			tz;
	char	   *tzn;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_NULL();

	if (timestamp2tm(timestamp, &tz, tm, &fsec, &tzn) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	result = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - POSTGRES_EPOCH_JDATE;

	PG_RETURN_DATEADT(result);
}


/* abstime_date()
 * Convert abstime to date data type.
 */
Datum
abstime_date(PG_FUNCTION_ARGS)
{
	AbsoluteTime abstime = PG_GETARG_ABSOLUTETIME(0);
	DateADT		result;
	struct tm	tt,
			   *tm = &tt;
	int			tz;

	switch (abstime)
	{
		case INVALID_ABSTIME:
		case NOSTART_ABSTIME:
		case NOEND_ABSTIME:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			   errmsg("cannot convert reserved abstime value to date")));

			/*
			 * pretend to drop through to make compiler think that result
			 * will be set
			 */

		default:
			abstime2tm(abstime, &tz, tm, NULL);
			result = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - POSTGRES_EPOCH_JDATE;
			break;
	}

	PG_RETURN_DATEADT(result);
}


/* date_text()
 * Convert date to text data type.
 */
Datum
date_text(PG_FUNCTION_ARGS)
{
	/* Input is a Date, but may as well leave it in Datum form */
	Datum		date = PG_GETARG_DATUM(0);
	text	   *result;
	char	   *str;
	int			len;

	str = DatumGetCString(DirectFunctionCall1(date_out, date));

	len = (strlen(str) + VARHDRSZ);

	result = palloc(len);

	VARATT_SIZEP(result) = len;
	memmove(VARDATA(result), str, (len - VARHDRSZ));

	pfree(str);

	PG_RETURN_TEXT_P(result);
}


/* text_date()
 * Convert text string to date.
 * Text type is not null terminated, so use temporary string
 *	then call the standard input routine.
 */
Datum
text_date(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_P(0);
	int			i;
	char	   *sp,
			   *dp,
				dstr[MAXDATELEN + 1];

	if (VARSIZE(str) - VARHDRSZ > MAXDATELEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("invalid input syntax for type date: \"%s\"",
						VARDATA(str))));

	sp = VARDATA(str);
	dp = dstr;
	for (i = 0; i < (VARSIZE(str) - VARHDRSZ); i++)
		*dp++ = *sp++;
	*dp = '\0';

	return DirectFunctionCall1(date_in,
							   CStringGetDatum(dstr));
}


/*****************************************************************************
 *	 Time ADT
 *****************************************************************************/

Datum
time_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	TimeADT		result;
	fsec_t		fsec;
	struct tm	tt,
			   *tm = &tt;
	int			tz;
	int			nf;
	int			dterr;
	char		workbuf[MAXDATELEN + 1];
	char	   *field[MAXDATEFIELDS];
	int			dtype;
	int			ftype[MAXDATEFIELDS];

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf),
						  field, ftype, MAXDATEFIELDS, &nf);
	if (dterr == 0)
		dterr = DecodeTimeOnly(field, ftype, nf, &dtype, tm, &fsec, &tz);
	if (dterr != 0)
		DateTimeParseError(dterr, str, "time");

	tm2time(tm, fsec, &result);
	AdjustTimeForTypmod(&result, typmod);

	PG_RETURN_TIMEADT(result);
}

/* tm2time()
 * Convert a tm structure to a time data type.
 */
static int
tm2time(struct tm * tm, fsec_t fsec, TimeADT *result)
{
#ifdef HAVE_INT64_TIMESTAMP
	*result = ((((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec)
				* INT64CONST(1000000)) + fsec);
#else
	*result = ((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec + fsec);
#endif
	return 0;
}

/* time2tm()
 * Convert time data type to POSIX time structure.
 * For dates within the system-supported time_t range, convert to the
 *	local time zone. If out of this range, leave as GMT. - tgl 97/05/27
 */
static int
time2tm(TimeADT time, struct tm * tm, fsec_t *fsec)
{
#ifdef HAVE_INT64_TIMESTAMP
	tm->tm_hour = (time / INT64CONST(3600000000));
	time -= (tm->tm_hour * INT64CONST(3600000000));
	tm->tm_min = (time / INT64CONST(60000000));
	time -= (tm->tm_min * INT64CONST(60000000));
	tm->tm_sec = (time / INT64CONST(1000000));
	time -= (tm->tm_sec * INT64CONST(1000000));
	*fsec = time;
#else
	double		trem;

	trem = time;
	TMODULO(trem, tm->tm_hour, 3600e0);
	TMODULO(trem, tm->tm_min, 60e0);
	TMODULO(trem, tm->tm_sec, 1e0);
	*fsec = trem;
#endif

	return 0;
}

Datum
time_out(PG_FUNCTION_ARGS)
{
	TimeADT		time = PG_GETARG_TIMEADT(0);
	char	   *result;
	struct tm	tt,
			   *tm = &tt;
	fsec_t		fsec;
	char		buf[MAXDATELEN + 1];

	time2tm(time, tm, &fsec);
	EncodeTimeOnly(tm, fsec, NULL, DateStyle, buf);

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

/*
 *		time_recv			- converts external binary format to time
 *
 * We make no attempt to provide compatibility between int and float
 * time representations ...
 */
Datum
time_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

#ifdef HAVE_INT64_TIMESTAMP
	PG_RETURN_TIMEADT((TimeADT) pq_getmsgint64(buf));
#else
	PG_RETURN_TIMEADT((TimeADT) pq_getmsgfloat8(buf));
#endif
}

/*
 *		time_send			- converts time to binary format
 */
Datum
time_send(PG_FUNCTION_ARGS)
{
	TimeADT		time = PG_GETARG_TIMEADT(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
#ifdef HAVE_INT64_TIMESTAMP
	pq_sendint64(&buf, time);
#else
	pq_sendfloat8(&buf, time);
#endif
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/* time_scale()
 * Adjust time type for specified scale factor.
 * Used by PostgreSQL type system to stuff columns.
 */
Datum
time_scale(PG_FUNCTION_ARGS)
{
	TimeADT		time = PG_GETARG_TIMEADT(0);
	int32		typmod = PG_GETARG_INT32(1);
	TimeADT		result;

	result = time;
	AdjustTimeForTypmod(&result, typmod);

	PG_RETURN_TIMEADT(result);
}

/* AdjustTimeForTypmod()
 * Force the precision of the time value to a specified value.
 * Uses *exactly* the same code as in AdjustTimestampForTypemod()
 * but we make a separate copy because those types do not
 * have a fundamental tie together but rather a coincidence of
 * implementation. - thomas
 */
static void
AdjustTimeForTypmod(TimeADT *time, int32 typmod)
{
#ifdef HAVE_INT64_TIMESTAMP
	static const int64 TimeScales[MAX_TIME_PRECISION + 1] = {
		INT64CONST(1000000),
		INT64CONST(100000),
		INT64CONST(10000),
		INT64CONST(1000),
		INT64CONST(100),
		INT64CONST(10),
		INT64CONST(1)
	};

	static const int64 TimeOffsets[MAX_TIME_PRECISION + 1] = {
		INT64CONST(500000),
		INT64CONST(50000),
		INT64CONST(5000),
		INT64CONST(500),
		INT64CONST(50),
		INT64CONST(5),
		INT64CONST(0)
	};

#else
	/* note MAX_TIME_PRECISION differs in this case */
	static const double TimeScales[MAX_TIME_PRECISION + 1] = {
		1.0,
		10.0,
		100.0,
		1000.0,
		10000.0,
		100000.0,
		1000000.0,
		10000000.0,
		100000000.0,
		1000000000.0,
		10000000000.0
	};
#endif

	if ((typmod >= 0) && (typmod <= MAX_TIME_PRECISION))
	{
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
			*time = (((*time + TimeOffsets[typmod]) / TimeScales[typmod])
					 * TimeScales[typmod]);
		}
		else
		{
			*time = -((((-*time) + TimeOffsets[typmod]) / TimeScales[typmod])
					  * TimeScales[typmod]);
		}
#else
		*time = (rint(((double) *time) * TimeScales[typmod])
				 / TimeScales[typmod]);
#endif
	}
}


Datum
time_eq(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_BOOL(time1 == time2);
}

Datum
time_ne(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_BOOL(time1 != time2);
}

Datum
time_lt(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_BOOL(time1 < time2);
}

Datum
time_le(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_BOOL(time1 <= time2);
}

Datum
time_gt(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_BOOL(time1 > time2);
}

Datum
time_ge(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_BOOL(time1 >= time2);
}

Datum
time_cmp(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	if (time1 < time2)
		PG_RETURN_INT32(-1);
	if (time1 > time2)
		PG_RETURN_INT32(1);
	PG_RETURN_INT32(0);
}

Datum
time_larger(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_TIMEADT((time1 > time2) ? time1 : time2);
}

Datum
time_smaller(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);

	PG_RETURN_TIMEADT((time1 < time2) ? time1 : time2);
}

/* overlaps_time() --- implements the SQL92 OVERLAPS operator.
 *
 * Algorithm is per SQL92 spec.  This is much harder than you'd think
 * because the spec requires us to deliver a non-null answer in some cases
 * where some of the inputs are null.
 */
Datum
overlaps_time(PG_FUNCTION_ARGS)
{
	/*
	 * The arguments are TimeADT, but we leave them as generic Datums to
	 * avoid dereferencing nulls (TimeADT is pass-by-reference!)
	 */
	Datum		ts1 = PG_GETARG_DATUM(0);
	Datum		te1 = PG_GETARG_DATUM(1);
	Datum		ts2 = PG_GETARG_DATUM(2);
	Datum		te2 = PG_GETARG_DATUM(3);
	bool		ts1IsNull = PG_ARGISNULL(0);
	bool		te1IsNull = PG_ARGISNULL(1);
	bool		ts2IsNull = PG_ARGISNULL(2);
	bool		te2IsNull = PG_ARGISNULL(3);

#define TIMEADT_GT(t1,t2) \
	(DatumGetTimeADT(t1) > DatumGetTimeADT(t2))
#define TIMEADT_LT(t1,t2) \
	(DatumGetTimeADT(t1) < DatumGetTimeADT(t2))

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
		if (TIMEADT_GT(ts1, te1))
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
		if (TIMEADT_GT(ts2, te2))
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
	if (TIMEADT_GT(ts1, ts2))
	{
		/*
		 * This case is ts1 < te2 OR te1 < te2, which may look redundant
		 * but in the presence of nulls it's not quite completely so.
		 */
		if (te2IsNull)
			PG_RETURN_NULL();
		if (TIMEADT_LT(ts1, te2))
			PG_RETURN_BOOL(true);
		if (te1IsNull)
			PG_RETURN_NULL();

		/*
		 * If te1 is not null then we had ts1 <= te1 above, and we just
		 * found ts1 >= te2, hence te1 >= te2.
		 */
		PG_RETURN_BOOL(false);
	}
	else if (TIMEADT_LT(ts1, ts2))
	{
		/* This case is ts2 < te1 OR te2 < te1 */
		if (te1IsNull)
			PG_RETURN_NULL();
		if (TIMEADT_LT(ts2, te1))
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

#undef TIMEADT_GT
#undef TIMEADT_LT
}

/* timestamp_time()
 * Convert timestamp to time data type.
 */
Datum
timestamp_time(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	TimeADT		result;
	struct tm	tt,
			   *tm = &tt;
	fsec_t		fsec;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_NULL();

	if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

#ifdef HAVE_INT64_TIMESTAMP

	/*
	 * Could also do this with time = (timestamp / 86400000000 *
	 * 86400000000) - timestamp;
	 */
	result = ((((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec)
			   * INT64CONST(1000000)) + fsec);
#else
	result = ((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec + fsec);
#endif

	PG_RETURN_TIMEADT(result);
}

/* timestamptz_time()
 * Convert timestamptz to time data type.
 */
Datum
timestamptz_time(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMP(0);
	TimeADT		result;
	struct tm	tt,
			   *tm = &tt;
	int			tz;
	fsec_t		fsec;
	char	   *tzn;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_NULL();

	if (timestamp2tm(timestamp, &tz, tm, &fsec, &tzn) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

#ifdef HAVE_INT64_TIMESTAMP

	/*
	 * Could also do this with time = (timestamp / 86400000000 *
	 * 86400000000) - timestamp;
	 */
	result = ((((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec)
			   * INT64CONST(1000000)) + fsec);
#else
	result = ((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec + fsec);
#endif

	PG_RETURN_TIMEADT(result);
}

/* datetime_timestamp()
 * Convert date and time to timestamp data type.
 */
Datum
datetime_timestamp(PG_FUNCTION_ARGS)
{
	DateADT		date = PG_GETARG_DATEADT(0);
	TimeADT		time = PG_GETARG_TIMEADT(1);
	Timestamp	result;

	result = DatumGetTimestamp(DirectFunctionCall1(date_timestamp,
												 DateADTGetDatum(date)));
	result += time;

	PG_RETURN_TIMESTAMP(result);
}

/* time_interval()
 * Convert time to interval data type.
 */
Datum
time_interval(PG_FUNCTION_ARGS)
{
	TimeADT		time = PG_GETARG_TIMEADT(0);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	result->time = time;
	result->month = 0;

	PG_RETURN_INTERVAL_P(result);
}

/* interval_time()
 * Convert interval to time data type.
 *
 * This is defined as producing the fractional-day portion of the interval.
 * Therefore, we can just ignore the months field.	It is not real clear
 * what to do with negative intervals, but we choose to subtract the floor,
 * so that, say, '-2 hours' becomes '22:00:00'.
 */
Datum
interval_time(PG_FUNCTION_ARGS)
{
	Interval   *span = PG_GETARG_INTERVAL_P(0);
	TimeADT		result;

#ifdef HAVE_INT64_TIMESTAMP
	int64		days;

	result = span->time;
	if (result >= INT64CONST(86400000000))
	{
		days = result / INT64CONST(86400000000);
		result -= days * INT64CONST(86400000000);
	}
	else if (result < 0)
	{
		days = (-result + INT64CONST(86400000000) - 1) / INT64CONST(86400000000);
		result += days * INT64CONST(86400000000);
	}
#else
	result = span->time;
	if (result >= 86400e0 || result < 0)
		result -= floor(result / 86400e0) * 86400e0;
#endif

	PG_RETURN_TIMEADT(result);
}

/* time_mi_time()
 * Subtract two times to produce an interval.
 */
Datum
time_mi_time(PG_FUNCTION_ARGS)
{
	TimeADT		time1 = PG_GETARG_TIMEADT(0);
	TimeADT		time2 = PG_GETARG_TIMEADT(1);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	result->time = (time1 - time2);
	result->month = 0;

	PG_RETURN_INTERVAL_P(result);
}

/* time_pl_interval()
 * Add interval to time.
 */
Datum
time_pl_interval(PG_FUNCTION_ARGS)
{
	TimeADT		time = PG_GETARG_TIMEADT(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	TimeADT		result;

#ifdef HAVE_INT64_TIMESTAMP
	result = (time + span->time);
	result -= (result / INT64CONST(86400000000) * INT64CONST(86400000000));
	if (result < INT64CONST(0))
		result += INT64CONST(86400000000);
#else
	TimeADT		time1;

	result = (time + span->time);
	TMODULO(result, time1, 86400e0);
	if (result < 0)
		result += 86400;
#endif

	PG_RETURN_TIMEADT(result);
}

/* time_mi_interval()
 * Subtract interval from time.
 */
Datum
time_mi_interval(PG_FUNCTION_ARGS)
{
	TimeADT		time = PG_GETARG_TIMEADT(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	TimeADT		result;

#ifdef HAVE_INT64_TIMESTAMP
	result = (time - span->time);
	result -= (result / INT64CONST(86400000000) * INT64CONST(86400000000));
	if (result < INT64CONST(0))
		result += INT64CONST(86400000000);
#else
	TimeADT		time1;

	result = (time - span->time);
	TMODULO(result, time1, 86400e0);
	if (result < 0)
		result += 86400;
#endif

	PG_RETURN_TIMEADT(result);
}

/* interval_pl_time()
 * Add time to interval.
 */
Datum
interval_pl_time(PG_FUNCTION_ARGS)
{
	Datum		span = PG_GETARG_DATUM(0);
	Datum		time = PG_GETARG_DATUM(1);

	return DirectFunctionCall2(time_pl_interval, time, span);
}


/* time_text()
 * Convert time to text data type.
 */
Datum
time_text(PG_FUNCTION_ARGS)
{
	/* Input is a Time, but may as well leave it in Datum form */
	Datum		time = PG_GETARG_DATUM(0);
	text	   *result;
	char	   *str;
	int			len;

	str = DatumGetCString(DirectFunctionCall1(time_out, time));

	len = (strlen(str) + VARHDRSZ);

	result = palloc(len);

	VARATT_SIZEP(result) = len;
	memmove(VARDATA(result), str, (len - VARHDRSZ));

	pfree(str);

	PG_RETURN_TEXT_P(result);
}


/* text_time()
 * Convert text string to time.
 * Text type is not null terminated, so use temporary string
 *	then call the standard input routine.
 */
Datum
text_time(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_P(0);
	int			i;
	char	   *sp,
			   *dp,
				dstr[MAXDATELEN + 1];

	if (VARSIZE(str) - VARHDRSZ > MAXDATELEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("invalid input syntax for type time: \"%s\"",
						VARDATA(str))));

	sp = VARDATA(str);
	dp = dstr;
	for (i = 0; i < (VARSIZE(str) - VARHDRSZ); i++)
		*dp++ = *sp++;
	*dp = '\0';

	return DirectFunctionCall3(time_in,
							   CStringGetDatum(dstr),
							   ObjectIdGetDatum(InvalidOid),
							   Int32GetDatum(-1));
}

/* time_part()
 * Extract specified field from time type.
 */
Datum
time_part(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_P(0);
	TimeADT		time = PG_GETARG_TIMEADT(1);
	float8		result;
	int			type,
				val;
	int			i;
	char	   *up,
			   *lp,
				lowunits[MAXDATELEN + 1];

	if (VARSIZE(units) - VARHDRSZ > MAXDATELEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"time\" units \"%s\" not recognized",
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
		fsec_t		fsec;
		struct tm	tt,
				   *tm = &tt;

		time2tm(time, tm, &fsec);

		switch (val)
		{
			case DTK_MICROSEC:
#ifdef HAVE_INT64_TIMESTAMP
				result = ((tm->tm_sec * INT64CONST(1000000)) + fsec);
#else
				result = ((tm->tm_sec + fsec) * 1000000);
#endif
				break;

			case DTK_MILLISEC:
#ifdef HAVE_INT64_TIMESTAMP
				result = ((tm->tm_sec * INT64CONST(1000))
						  + (fsec / INT64CONST(1000)));
#else
				result = ((tm->tm_sec + fsec) * 1000);
#endif
				break;

			case DTK_SECOND:
#ifdef HAVE_INT64_TIMESTAMP
				result = (tm->tm_sec + (fsec / INT64CONST(1000000)));
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

			case DTK_TZ:
			case DTK_TZ_MINUTE:
			case DTK_TZ_HOUR:
			case DTK_DAY:
			case DTK_MONTH:
			case DTK_QUARTER:
			case DTK_YEAR:
			case DTK_DECADE:
			case DTK_CENTURY:
			case DTK_MILLENNIUM:
			default:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("\"time\" units \"%s\" not recognized",
							 DatumGetCString(DirectFunctionCall1(textout,
											 PointerGetDatum(units))))));

				result = 0;
		}
	}
	else if ((type == RESERV) && (val == DTK_EPOCH))
	{
#ifdef HAVE_INT64_TIMESTAMP
		result = (time / 1000000e0);
#else
		result = time;
#endif
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"time\" units \"%s\" not recognized",
						DatumGetCString(DirectFunctionCall1(textout,
											 PointerGetDatum(units))))));
		result = 0;
	}

	PG_RETURN_FLOAT8(result);
}


/*****************************************************************************
 *	 Time With Time Zone ADT
 *****************************************************************************/

/* tm2timetz()
 * Convert a tm structure to a time data type.
 */
static int
tm2timetz(struct tm * tm, fsec_t fsec, int tz, TimeTzADT *result)
{
#ifdef HAVE_INT64_TIMESTAMP
	result->time = ((((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec)
					 * INT64CONST(1000000)) + fsec);
#else
	result->time = ((((tm->tm_hour * 60) + tm->tm_min) * 60) + tm->tm_sec + fsec);
#endif
	result->zone = tz;

	return 0;
}

Datum
timetz_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	TimeTzADT  *result;
	fsec_t		fsec;
	struct tm	tt,
			   *tm = &tt;
	int			tz;
	int			nf;
	int			dterr;
	char		workbuf[MAXDATELEN + 1];
	char	   *field[MAXDATEFIELDS];
	int			dtype;
	int			ftype[MAXDATEFIELDS];

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf),
						  field, ftype, MAXDATEFIELDS, &nf);
	if (dterr == 0)
		dterr = DecodeTimeOnly(field, ftype, nf, &dtype, tm, &fsec, &tz);
	if (dterr != 0)
		DateTimeParseError(dterr, str, "time with time zone");

	result = (TimeTzADT *) palloc(sizeof(TimeTzADT));
	tm2timetz(tm, fsec, tz, result);
	AdjustTimeForTypmod(&(result->time), typmod);

	PG_RETURN_TIMETZADT_P(result);
}

Datum
timetz_out(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time = PG_GETARG_TIMETZADT_P(0);
	char	   *result;
	struct tm	tt,
			   *tm = &tt;
	fsec_t		fsec;
	int			tz;
	char		buf[MAXDATELEN + 1];

	timetz2tm(time, tm, &fsec, &tz);
	EncodeTimeOnly(tm, fsec, &tz, DateStyle, buf);

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

/*
 *		timetz_recv			- converts external binary format to timetz
 */
Datum
timetz_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	TimeTzADT  *time;

	time = (TimeTzADT *) palloc(sizeof(TimeTzADT));

#ifdef HAVE_INT64_TIMESTAMP
	time->time = pq_getmsgint64(buf);
#else
	time->time = pq_getmsgfloat8(buf);
#endif
	time->zone = pq_getmsgint(buf, sizeof(time->zone));

	PG_RETURN_TIMETZADT_P(time);
}

/*
 *		timetz_send			- converts timetz to binary format
 */
Datum
timetz_send(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time = PG_GETARG_TIMETZADT_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
#ifdef HAVE_INT64_TIMESTAMP
	pq_sendint64(&buf, time->time);
#else
	pq_sendfloat8(&buf, time->time);
#endif
	pq_sendint(&buf, time->zone, sizeof(time->zone));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/* timetz2tm()
 * Convert TIME WITH TIME ZONE data type to POSIX time structure.
 */
static int
timetz2tm(TimeTzADT *time, struct tm * tm, fsec_t *fsec, int *tzp)
{
#ifdef HAVE_INT64_TIMESTAMP
	int64		trem = time->time;

	tm->tm_hour = (trem / INT64CONST(3600000000));
	trem -= (tm->tm_hour * INT64CONST(3600000000));
	tm->tm_min = (trem / INT64CONST(60000000));
	trem -= (tm->tm_min * INT64CONST(60000000));
	tm->tm_sec = (trem / INT64CONST(1000000));
	*fsec = (trem - (tm->tm_sec * INT64CONST(1000000)));
#else
	double		trem = time->time;

	TMODULO(trem, tm->tm_hour, 3600e0);
	TMODULO(trem, tm->tm_min, 60e0);
	TMODULO(trem, tm->tm_sec, 1e0);
	*fsec = trem;
#endif

	if (tzp != NULL)
		*tzp = time->zone;

	return 0;
}

/* timetz_scale()
 * Adjust time type for specified scale factor.
 * Used by PostgreSQL type system to stuff columns.
 */
Datum
timetz_scale(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time = PG_GETARG_TIMETZADT_P(0);
	int32		typmod = PG_GETARG_INT32(1);
	TimeTzADT  *result;

	result = (TimeTzADT *) palloc(sizeof(TimeTzADT));

	result->time = time->time;
	result->zone = time->zone;

	AdjustTimeForTypmod(&(result->time), typmod);

	PG_RETURN_TIMETZADT_P(result);
}


static int
timetz_cmp_internal(TimeTzADT *time1, TimeTzADT *time2)
{
	/* Primary sort is by true (GMT-equivalent) time */
#ifdef HAVE_INT64_TIMESTAMP
	int64		t1,
				t2;

	t1 = time1->time + (time1->zone * INT64CONST(1000000));
	t2 = time2->time + (time2->zone * INT64CONST(1000000));
#else
	double		t1,
				t2;

	t1 = time1->time + time1->zone;
	t2 = time2->time + time2->zone;
#endif

	if (t1 > t2)
		return 1;
	if (t1 < t2)
		return -1;

	/*
	 * If same GMT time, sort by timezone; we only want to say that two
	 * timetz's are equal if both the time and zone parts are equal.
	 */
	if (time1->zone > time2->zone)
		return 1;
	if (time1->zone < time2->zone)
		return -1;

	return 0;
}

Datum
timetz_eq(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_BOOL(timetz_cmp_internal(time1, time2) == 0);
}

Datum
timetz_ne(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_BOOL(timetz_cmp_internal(time1, time2) != 0);
}

Datum
timetz_lt(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_BOOL(timetz_cmp_internal(time1, time2) < 0);
}

Datum
timetz_le(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_BOOL(timetz_cmp_internal(time1, time2) <= 0);
}

Datum
timetz_gt(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_BOOL(timetz_cmp_internal(time1, time2) > 0);
}

Datum
timetz_ge(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_BOOL(timetz_cmp_internal(time1, time2) >= 0);
}

Datum
timetz_cmp(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *time2 = PG_GETARG_TIMETZADT_P(1);

	PG_RETURN_INT32(timetz_cmp_internal(time1, time2));
}

/*
 * timetz, being an unusual size, needs a specialized hash function.
 */
Datum
timetz_hash(PG_FUNCTION_ARGS)
{
	TimeTzADT  *key = PG_GETARG_TIMETZADT_P(0);

	/*
	 * Specify hash length as sizeof(double) + sizeof(int4), not as
	 * sizeof(TimeTzADT), so that any garbage pad bytes in the structure
	 * won't be included in the hash!
	 */
	return hash_any((unsigned char *) key, sizeof(key->time) + sizeof(key->zone));
}

Datum
timetz_larger(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *time2 = PG_GETARG_TIMETZADT_P(1);
	TimeTzADT  *result;

	if (timetz_cmp_internal(time1, time2) > 0)
		result = time1;
	else
		result = time2;
	PG_RETURN_TIMETZADT_P(result);
}

Datum
timetz_smaller(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time1 = PG_GETARG_TIMETZADT_P(0);
	TimeTzADT  *time2 = PG_GETARG_TIMETZADT_P(1);
	TimeTzADT  *result;

	if (timetz_cmp_internal(time1, time2) < 0)
		result = time1;
	else
		result = time2;
	PG_RETURN_TIMETZADT_P(result);
}

/* timetz_pl_interval()
 * Add interval to timetz.
 */
Datum
timetz_pl_interval(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time = PG_GETARG_TIMETZADT_P(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	TimeTzADT  *result;

#ifndef HAVE_INT64_TIMESTAMP
	TimeTzADT	time1;
#endif

	result = (TimeTzADT *) palloc(sizeof(TimeTzADT));

#ifdef HAVE_INT64_TIMESTAMP
	result->time = (time->time + span->time);
	result->time -= (result->time / INT64CONST(86400000000) * INT64CONST(86400000000));
	if (result->time < INT64CONST(0))
		result->time += INT64CONST(86400000000);
#else
	result->time = (time->time + span->time);
	TMODULO(result->time, time1.time, 86400e0);
	if (result->time < 0)
		result->time += 86400;
#endif

	result->zone = time->zone;

	PG_RETURN_TIMETZADT_P(result);
}

/* timetz_mi_interval()
 * Subtract interval from timetz.
 */
Datum
timetz_mi_interval(PG_FUNCTION_ARGS)
{
	TimeTzADT  *time = PG_GETARG_TIMETZADT_P(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	TimeTzADT  *result;

#ifndef HAVE_INT64_TIMESTAMP
	TimeTzADT	time1;
#endif

	result = (TimeTzADT *) palloc(sizeof(TimeTzADT));

#ifdef HAVE_INT64_TIMESTAMP
	result->time = (time->time - span->time);
	result->time -= (result->time / INT64CONST(86400000000) * INT64CONST(86400000000));
	if (result->time < INT64CONST(0))
		result->time += INT64CONST(86400000000);
#else
	result->time = (time->time - span->time);
	TMODULO(result->time, time1.time, 86400e0);
	if (result->time < 0)
		result->time += 86400;
#endif

	result->zone = time->zone;

	PG_RETURN_TIMETZADT_P(result);
}

/* overlaps_timetz() --- implements the SQL92 OVERLAPS operator.
 *
 * Algorithm is per SQL92 spec.  This is much harder than you'd think
 * because the spec requires us to deliver a non-null answer in some cases
 * where some of the inputs are null.
 */
Datum
overlaps_timetz(PG_FUNCTION_ARGS)
{
	/*
	 * The arguments are TimeTzADT *, but we leave them as generic Datums
	 * for convenience of notation --- and to avoid dereferencing nulls.
	 */
	Datum		ts1 = PG_GETARG_DATUM(0);
	Datum		te1 = PG_GETARG_DATUM(1);
	Datum		ts2 = PG_GETARG_DATUM(2);
	Datum		te2 = PG_GETARG_DATUM(3);
	bool		ts1IsNull = PG_ARGISNULL(0);
	bool		te1IsNull = PG_ARGISNULL(1);
	bool		ts2IsNull = PG_ARGISNULL(2);
	bool		te2IsNull = PG_ARGISNULL(3);

#define TIMETZ_GT(t1,t2) \
	DatumGetBool(DirectFunctionCall2(timetz_gt,t1,t2))
#define TIMETZ_LT(t1,t2) \
	DatumGetBool(DirectFunctionCall2(timetz_lt,t1,t2))

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
		if (TIMETZ_GT(ts1, te1))
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
		if (TIMETZ_GT(ts2, te2))
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
	if (TIMETZ_GT(ts1, ts2))
	{
		/*
		 * This case is ts1 < te2 OR te1 < te2, which may look redundant
		 * but in the presence of nulls it's not quite completely so.
		 */
		if (te2IsNull)
			PG_RETURN_NULL();
		if (TIMETZ_LT(ts1, te2))
			PG_RETURN_BOOL(true);
		if (te1IsNull)
			PG_RETURN_NULL();

		/*
		 * If te1 is not null then we had ts1 <= te1 above, and we just
		 * found ts1 >= te2, hence te1 >= te2.
		 */
		PG_RETURN_BOOL(false);
	}
	else if (TIMETZ_LT(ts1, ts2))
	{
		/* This case is ts2 < te1 OR te2 < te1 */
		if (te1IsNull)
			PG_RETURN_NULL();
		if (TIMETZ_LT(ts2, te1))
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

#undef TIMETZ_GT
#undef TIMETZ_LT
}


Datum
timetz_time(PG_FUNCTION_ARGS)
{
	TimeTzADT  *timetz = PG_GETARG_TIMETZADT_P(0);
	TimeADT		result;

	/* swallow the time zone and just return the time */
	result = timetz->time;

	PG_RETURN_TIMEADT(result);
}


Datum
time_timetz(PG_FUNCTION_ARGS)
{
	TimeADT		time = PG_GETARG_TIMEADT(0);
	TimeTzADT  *result;
	struct tm	tt,
			   *tm = &tt;
	fsec_t		fsec;
	int			tz;

	GetCurrentDateTime(tm);
	time2tm(time, tm, &fsec);
	tz = DetermineLocalTimeZone(tm);

	result = (TimeTzADT *) palloc(sizeof(TimeTzADT));

	result->time = time;
	result->zone = tz;

	PG_RETURN_TIMETZADT_P(result);
}


/* timestamptz_timetz()
 * Convert timestamp to timetz data type.
 */
Datum
timestamptz_timetz(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMP(0);
	TimeTzADT  *result;
	struct tm	tt,
			   *tm = &tt;
	int			tz;
	fsec_t		fsec;
	char	   *tzn;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_NULL();

	if (timestamp2tm(timestamp, &tz, tm, &fsec, &tzn) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	result = (TimeTzADT *) palloc(sizeof(TimeTzADT));

	tm2timetz(tm, fsec, tz, result);

	PG_RETURN_TIMETZADT_P(result);
}


/* datetimetz_timestamptz()
 * Convert date and timetz to timestamp with time zone data type.
 * Timestamp is stored in GMT, so add the time zone
 * stored with the timetz to the result.
 * - thomas 2000-03-10
 */
Datum
datetimetz_timestamptz(PG_FUNCTION_ARGS)
{
	DateADT		date = PG_GETARG_DATEADT(0);
	TimeTzADT  *time = PG_GETARG_TIMETZADT_P(1);
	TimestampTz result;

#ifdef HAVE_INT64_TIMESTAMP
	result = (((date * INT64CONST(86400000000)) + time->time)
			  + (time->zone * INT64CONST(1000000)));
#else
	result = (((date * 86400.0) + time->time) + time->zone);
#endif

	PG_RETURN_TIMESTAMP(result);
}


/* timetz_text()
 * Convert timetz to text data type.
 */
Datum
timetz_text(PG_FUNCTION_ARGS)
{
	/* Input is a Timetz, but may as well leave it in Datum form */
	Datum		timetz = PG_GETARG_DATUM(0);
	text	   *result;
	char	   *str;
	int			len;

	str = DatumGetCString(DirectFunctionCall1(timetz_out, timetz));

	len = (strlen(str) + VARHDRSZ);

	result = palloc(len);

	VARATT_SIZEP(result) = len;
	memmove(VARDATA(result), str, (len - VARHDRSZ));

	pfree(str);

	PG_RETURN_TEXT_P(result);
}


/* text_timetz()
 * Convert text string to timetz.
 * Text type is not null terminated, so use temporary string
 *	then call the standard input routine.
 */
Datum
text_timetz(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_P(0);
	int			i;
	char	   *sp,
			   *dp,
				dstr[MAXDATELEN + 1];

	if (VARSIZE(str) - VARHDRSZ > MAXDATELEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
		   errmsg("invalid input syntax for type time with time zone: \"%s\"",
				  VARDATA(str))));

	sp = VARDATA(str);
	dp = dstr;
	for (i = 0; i < (VARSIZE(str) - VARHDRSZ); i++)
		*dp++ = *sp++;
	*dp = '\0';

	return DirectFunctionCall3(timetz_in,
							   CStringGetDatum(dstr),
							   ObjectIdGetDatum(InvalidOid),
							   Int32GetDatum(-1));
}

/* timetz_part()
 * Extract specified field from time type.
 */
Datum
timetz_part(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_P(0);
	TimeTzADT  *time = PG_GETARG_TIMETZADT_P(1);
	float8		result;
	int			type,
				val;
	int			i;
	char	   *up,
			   *lp,
				lowunits[MAXDATELEN + 1];

	if (VARSIZE(units) - VARHDRSZ > MAXDATELEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"time with time zone\" units \"%s\" not recognized",
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
		double		dummy;
		int			tz;
		fsec_t		fsec;
		struct tm	tt,
				   *tm = &tt;

		timetz2tm(time, tm, &fsec, &tz);

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
				result = ((tm->tm_sec * INT64CONST(1000000)) + fsec);
#else
				result = ((tm->tm_sec + fsec) * 1000000);
#endif
				break;

			case DTK_MILLISEC:
#ifdef HAVE_INT64_TIMESTAMP
				result = ((tm->tm_sec * INT64CONST(1000))
						  + (fsec / INT64CONST(1000)));
#else
				result = ((tm->tm_sec + fsec) * 1000);
#endif
				break;

			case DTK_SECOND:
#ifdef HAVE_INT64_TIMESTAMP
				result = (tm->tm_sec + (fsec / INT64CONST(1000000)));
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
			case DTK_MONTH:
			case DTK_QUARTER:
			case DTK_YEAR:
			case DTK_DECADE:
			case DTK_CENTURY:
			case DTK_MILLENNIUM:
			default:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("\"time with time zone\" units \"%s\" not recognized",
							 DatumGetCString(DirectFunctionCall1(textout,
											 PointerGetDatum(units))))));

				result = 0;
		}
	}
	else if ((type == RESERV) && (val == DTK_EPOCH))
	{
#ifdef HAVE_INT64_TIMESTAMP
		result = ((time->time / 1000000e0) + time->zone);
#else
		result = (time->time + time->zone);
#endif
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"time with time zone\" units \"%s\" not recognized",
						DatumGetCString(DirectFunctionCall1(textout,
											 PointerGetDatum(units))))));

		result = 0;
	}

	PG_RETURN_FLOAT8(result);
}

/* timetz_zone()
 * Encode time with time zone type with specified time zone.
 */
Datum
timetz_zone(PG_FUNCTION_ARGS)
{
	text	   *zone = PG_GETARG_TEXT_P(0);
	TimeTzADT  *time = PG_GETARG_TIMETZADT_P(1);
	TimeTzADT  *result;
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

	result = (TimeTzADT *) palloc(sizeof(TimeTzADT));

	if ((type == TZ) || (type == DTZ))
	{
		tz = val * 60;
#ifdef HAVE_INT64_TIMESTAMP
		result->time = time->time + ((time->zone - tz) * INT64CONST(1000000));
		while (result->time < INT64CONST(0))
			result->time += INT64CONST(86400000000);
		while (result->time >= INT64CONST(86400000000))
			result->time -= INT64CONST(86400000000);
#else
		result->time = time->time + (time->zone - tz);
		while (result->time < 0)
			result->time += 86400;
		while (result->time >= 86400)
			result->time -= 86400;
#endif

		result->zone = tz;
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("time zone \"%s\" not recognized", lowzone)));

		PG_RETURN_NULL();
	}

	PG_RETURN_TIMETZADT_P(result);
}	/* timetz_zone() */

/* timetz_izone()
 * Encode time with time zone type with specified time interval as time zone.
 */
Datum
timetz_izone(PG_FUNCTION_ARGS)
{
	Interval   *zone = PG_GETARG_INTERVAL_P(0);
	TimeTzADT  *time = PG_GETARG_TIMETZADT_P(1);
	TimeTzADT  *result;
	int			tz;

	if (zone->month != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"interval\" time zone \"%s\" not valid",
						DatumGetCString(DirectFunctionCall1(interval_out,
											  PointerGetDatum(zone))))));

#ifdef HAVE_INT64_TIMESTAMP
	tz = -(zone->time / INT64CONST(1000000));
#else
	tz = -(zone->time);
#endif

	result = (TimeTzADT *) palloc(sizeof(TimeTzADT));

#ifdef HAVE_INT64_TIMESTAMP
	result->time = time->time + ((time->zone - tz) * INT64CONST(1000000));
	while (result->time < INT64CONST(0))
		result->time += INT64CONST(86400000000);
	while (result->time >= INT64CONST(86400000000))
		result->time -= INT64CONST(86400000000);
#else
	result->time = time->time + (time->zone - tz);
	while (result->time < 0)
		result->time += 86400;
	while (result->time >= 86400)
		result->time -= 86400;
#endif

	result->zone = tz;

	PG_RETURN_TIMETZADT_P(result);
}	/* timetz_izone() */
