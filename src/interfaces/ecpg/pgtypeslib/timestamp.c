#include <math.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <float.h>
#include <stdio.h>

#ifdef __FAST_MATH__
#error -ffast-math is known to break this code
#endif

#include "dt.h"
#include "extern.h"
#include "pgtypes_error.h"
#include "pgtypes_timestamp.h"

#ifdef HAVE_INT64_TIMESTAMP
static int64
time2t(const int hour, const int min, const int sec, const fsec_t fsec)
{
        return ((((((hour * 60) + min) * 60) + sec) * INT64CONST(1000000)) + fsec);
}       /* time2t() */

#else
static double
time2t(const int hour, const int min, const int sec, const fsec_t fsec)
{
        return ((((hour * 60) + min) * 60) + sec + fsec);
}       /* time2t() */
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
}       /* dt2local() */

/* tm2timestamp()
 * Convert a tm structure to a timestamp data type.
 * Note that year is _not_ 1900-based, but is an explicit full value.
 * Also, month is one-based, _not_ zero-based.
 */
static int
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

	date = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - date2j(2000, 1, 1);
	time = time2t(tm->tm_hour, tm->tm_min, tm->tm_sec, fsec);
#ifdef HAVE_INT64_TIMESTAMP
	*result = ((date * INT64CONST(86400000000)) + time);
	if ((*result < 0 && date >= 0) || (*result >= 0 && date < 0))
		elog(ERROR, "TIMESTAMP out of range '%04d-%02d-%02d'",
			tm->tm_year, tm->tm_mon, tm->tm_mday);
#else
	*result = ((date * 86400) + time);
#endif
	if (tzp != NULL)
		*result = dt2local(*result, -(*tzp));

	return 0;
}	/* tm2timestamp() */

static Timestamp
SetEpochTimestamp(void)
{
        Timestamp       dt;
        struct tm       tt, *tm = &tt;

	GetEpochTime(tm);
	tm2timestamp(tm, 0, NULL, &dt);
        return dt;
}       /* SetEpochTimestamp() */

static void
dt2time(Timestamp jd, int *hour, int *min, int *sec, fsec_t *fsec)
{
#ifdef HAVE_INT64_TIMESTAMP
        int64           time;
#else
        double          time;
#endif

        time = jd;

#ifdef HAVE_INT64_TIMESTAMP
        *hour = (time / INT64CONST(3600000000));
        time -= ((*hour) * INT64CONST(3600000000));
        *min = (time / INT64CONST(60000000));
        time -= ((*min) * INT64CONST(60000000));
        *sec = (time / INT64CONST(1000000));
        *fsec = (time - (*sec * INT64CONST(1000000)));
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
}       /* dt2time() */

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
static int
timestamp2tm(Timestamp dt, int *tzp, struct tm * tm, fsec_t *fsec, char **tzn)
{
#ifdef HAVE_INT64_TIMESTAMP
	int			date,
				date0;
	int64		time;

#else
	double		date,
				date0;
	double		time;
#endif
	time_t		utime;

#if defined(HAVE_TM_ZONE) || defined(HAVE_INT_TIMEZONE)
	struct tm  *tx;
#endif

	date0 = date2j(2000, 1, 1);

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
	if (date < -date0)
		return -1;

	/* add offset to go from J2000 back to standard Julian date */
	date += date0;

	j2date((int) date, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
	dt2time(time, &tm->tm_hour, &tm->tm_min, &tm->tm_sec, fsec);

	if (tzp != NULL)
	{
		/*
		 * Does this fall within the capabilities of the localtime()
		 * interface? Then use this to rotate to the local time zone.
		 */
		if (IS_VALID_UTIME(tm->tm_year, tm->tm_mon, tm->tm_mday))
		{
#ifdef HAVE_INT64_TIMESTAMP
			utime = ((dt / INT64CONST(1000000))
				   + ((date0 - date2j(1970, 1, 1)) * INT64CONST(86400)));
#else
			utime = (dt + ((date0 - date2j(1970, 1, 1)) * 86400));
#endif

#if defined(HAVE_TM_ZONE) || defined(HAVE_INT_TIMEZONE)
			tx = localtime(&utime);
			tm->tm_year = tx->tm_year + 1900;
			tm->tm_mon = tx->tm_mon + 1;
			tm->tm_mday = tx->tm_mday;
			tm->tm_hour = tx->tm_hour;
			tm->tm_min = tx->tm_min;
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

			dt = dt2local(dt, *tzp);
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
}	/* timestamp2tm() */

/* EncodeSpecialTimestamp()
 *  * Convert reserved timestamp data type to string.
 *   */
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
}       /* EncodeSpecialTimestamp() */

Timestamp
PGTYPEStimestamp_atot(char *str, char **endptr)
{
	Timestamp	result;
#ifdef HAVE_INT64_TIMESTAMP
	int64		noresult = 0;
#else
	double		noresult = 0.0;
#endif
	fsec_t		fsec;
	struct tm	tt, *tm = &tt;
	int			tz;
	int			dtype;
	int			nf;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		lowstr[MAXDATELEN + MAXDATEFIELDS];
        char            *realptr;
	char **ptr = (endptr != NULL) ? endptr : &realptr;

	errno = 0;
	if (strlen(str) >= sizeof(lowstr))
	{
		errno = PGTYPES_BAD_TIMESTAMP;
		return (noresult);
	}

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf, ptr) != 0)
	  || (DecodeDateTime(field, ftype, nf, &dtype, tm, &fsec, &tz, 0) != 0))
	{
                errno = PGTYPES_BAD_TIMESTAMP;
                return (noresult);
        }
	
	switch (dtype)
	{
		case DTK_DATE:
			if (tm2timestamp(tm, fsec, NULL, &result) != 0)
			{
				errno = PGTYPES_BAD_TIMESTAMP;
				return (noresult);;
			}
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
			errno = PGTYPES_BAD_TIMESTAMP;
			return (noresult);

		default:
			errno = PGTYPES_BAD_TIMESTAMP;
			return (noresult);
	}

	/* AdjustTimestampForTypmod(&result, typmod); */

	return result;
}

char *
PGTYPEStimestamp_ttoa(Timestamp tstamp)
{
        struct tm       tt, *tm = &tt;
        char            buf[MAXDATELEN + 1];
	char       *tzn = NULL;
	fsec_t          fsec;
	int		DateStyle = 0;

	if (TIMESTAMP_NOT_FINITE(tstamp))
                EncodeSpecialTimestamp(tstamp, buf);
        else if (timestamp2tm(tstamp, NULL, tm, &fsec, NULL) == 0)
                EncodeDateTime(tm, fsec, NULL, &tzn, DateStyle, buf, 0);
        else
	{
		errno = PGTYPES_BAD_TIMESTAMP;
		return NULL;
	}
        return pgtypes_strdup(buf);
}

