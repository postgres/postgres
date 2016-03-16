/*
 * src/interfaces/ecpg/pgtypeslib/timestamp.c
 */
#include "postgres_fe.h"

#include <time.h>
#include <float.h>
#include <limits.h>
#include <math.h>

#ifdef __FAST_MATH__
#error -ffast-math is known to break this code
#endif

#include "extern.h"
#include "dt.h"
#include "pgtypes_timestamp.h"
#include "pgtypes_date.h"


#ifdef HAVE_INT64_TIMESTAMP
static int64
time2t(const int hour, const int min, const int sec, const fsec_t fsec)
{
	return (((((hour * MINS_PER_HOUR) + min) * SECS_PER_MINUTE) + sec) * USECS_PER_SEC) + fsec;
}	/* time2t() */
#else
static double
time2t(const int hour, const int min, const int sec, const fsec_t fsec)
{
	return (((hour * MINS_PER_HOUR) + min) * SECS_PER_MINUTE) + sec + fsec;
}	/* time2t() */
#endif

static timestamp
dt2local(timestamp dt, int tz)
{
#ifdef HAVE_INT64_TIMESTAMP
	dt -= (tz * USECS_PER_SEC);
#else
	dt -= tz;
#endif
	return dt;
}	/* dt2local() */

/* tm2timestamp()
 * Convert a tm structure to a timestamp data type.
 * Note that year is _not_ 1900-based, but is an explicit full value.
 * Also, month is one-based, _not_ zero-based.
 *
 * Returns -1 on failure (overflow).
 */
int
tm2timestamp(struct tm * tm, fsec_t fsec, int *tzp, timestamp * result)
{
#ifdef HAVE_INT64_TIMESTAMP
	int			dDate;
	int64		time;
#else
	double		dDate,
				time;
#endif

	/* Prevent overflow in Julian-day routines */
	if (!IS_VALID_JULIAN(tm->tm_year, tm->tm_mon, tm->tm_mday))
		return -1;

	dDate = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - date2j(2000, 1, 1);
	time = time2t(tm->tm_hour, tm->tm_min, tm->tm_sec, fsec);
#ifdef HAVE_INT64_TIMESTAMP
	*result = (dDate * USECS_PER_DAY) + time;
	/* check for major overflow */
	if ((*result - time) / USECS_PER_DAY != dDate)
		return -1;
	/* check for just-barely overflow (okay except time-of-day wraps) */
	/* caution: we want to allow 1999-12-31 24:00:00 */
	if ((*result < 0 && dDate > 0) ||
		(*result > 0 && dDate < -1))
		return -1;
#else
	*result = dDate * SECS_PER_DAY + time;
#endif
	if (tzp != NULL)
		*result = dt2local(*result, -(*tzp));

	/* final range check catches just-out-of-range timestamps */
	if (!IS_VALID_TIMESTAMP(*result))
		return -1;

	return 0;
}	/* tm2timestamp() */

static timestamp
SetEpochTimestamp(void)
{
#ifdef HAVE_INT64_TIMESTAMP
	int64		noresult = 0;
#else
	double		noresult = 0.0;
#endif
	timestamp	dt;
	struct tm	tt,
			   *tm = &tt;

	if (GetEpochTime(tm) < 0)
		return noresult;

	tm2timestamp(tm, 0, NULL, &dt);
	return dt;
}	/* SetEpochTimestamp() */

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
timestamp2tm(timestamp dt, int *tzp, struct tm * tm, fsec_t *fsec, const char **tzn)
{
#ifdef HAVE_INT64_TIMESTAMP
	int64		dDate,
				date0;
	int64		time;
#else
	double		dDate,
				date0;
	double		time;
#endif
#if defined(HAVE_TM_ZONE) || defined(HAVE_INT_TIMEZONE)
	time_t		utime;
	struct tm  *tx;
#endif

	date0 = date2j(2000, 1, 1);

#ifdef HAVE_INT64_TIMESTAMP
	time = dt;
	TMODULO(time, dDate, USECS_PER_DAY);

	if (time < INT64CONST(0))
	{
		time += USECS_PER_DAY;
		dDate -= 1;
	}

	/* add offset to go from J2000 back to standard Julian date */
	dDate += date0;

	/* Julian day routine does not work for negative Julian days */
	if (dDate < 0 || dDate > (timestamp) INT_MAX)
		return -1;

	j2date((int) dDate, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
	dt2time(time, &tm->tm_hour, &tm->tm_min, &tm->tm_sec, fsec);
#else
	time = dt;
	TMODULO(time, dDate, (double) SECS_PER_DAY);

	if (time < 0)
	{
		time += SECS_PER_DAY;
		dDate -= 1;
	}

	/* add offset to go from J2000 back to standard Julian date */
	dDate += date0;

recalc_d:
	/* Julian day routine does not work for negative Julian days */
	if (dDate < 0 || dDate > (timestamp) INT_MAX)
		return -1;

	j2date((int) dDate, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
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
			dDate += 1;
			goto recalc_d;
		}
		goto recalc_t;
	}
#endif

	if (tzp != NULL)
	{
		/*
		 * Does this fall within the capabilities of the localtime()
		 * interface? Then use this to rotate to the local time zone.
		 */
		if (IS_VALID_UTIME(tm->tm_year, tm->tm_mon, tm->tm_mday))
		{
#if defined(HAVE_TM_ZONE) || defined(HAVE_INT_TIMEZONE)

#ifdef HAVE_INT64_TIMESTAMP
			utime = dt / USECS_PER_SEC +
				((date0 - date2j(1970, 1, 1)) * INT64CONST(86400));
#else
			utime = dt + (date0 - date2j(1970, 1, 1)) * SECS_PER_DAY;
#endif

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

			*tzp = -tm->tm_gmtoff;		/* tm_gmtoff is Sun/DEC-ism */
			if (tzn != NULL)
				*tzn = tm->tm_zone;
#elif defined(HAVE_INT_TIMEZONE)
			*tzp = (tm->tm_isdst > 0) ? TIMEZONE_GLOBAL - SECS_PER_HOUR : TIMEZONE_GLOBAL;
			if (tzn != NULL)
				*tzn = TZNAME_GLOBAL[(tm->tm_isdst > 0)];
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

	tm->tm_yday = dDate - date2j(tm->tm_year, 1, 1) + 1;

	return 0;
}	/* timestamp2tm() */

/* EncodeSpecialTimestamp()
 *	* Convert reserved timestamp data type to string.
 *	 */
static int
EncodeSpecialTimestamp(timestamp dt, char *str)
{
	if (TIMESTAMP_IS_NOBEGIN(dt))
		strcpy(str, EARLY);
	else if (TIMESTAMP_IS_NOEND(dt))
		strcpy(str, LATE);
	else
		return FALSE;

	return TRUE;
}	/* EncodeSpecialTimestamp() */

timestamp
PGTYPEStimestamp_from_asc(char *str, char **endptr)
{
	timestamp	result;

#ifdef HAVE_INT64_TIMESTAMP
	int64		noresult = 0;
#else
	double		noresult = 0.0;
#endif
	fsec_t		fsec;
	struct tm	tt,
			   *tm = &tt;
	int			dtype;
	int			nf;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		lowstr[MAXDATELEN + MAXDATEFIELDS];
	char	   *realptr;
	char	  **ptr = (endptr != NULL) ? endptr : &realptr;

	if (strlen(str) > MAXDATELEN)
	{
		errno = PGTYPES_TS_BAD_TIMESTAMP;
		return (noresult);
	}

	if (ParseDateTime(str, lowstr, field, ftype, &nf, ptr) != 0 ||
		DecodeDateTime(field, ftype, nf, &dtype, tm, &fsec, 0) != 0)
	{
		errno = PGTYPES_TS_BAD_TIMESTAMP;
		return (noresult);
	}

	switch (dtype)
	{
		case DTK_DATE:
			if (tm2timestamp(tm, fsec, NULL, &result) != 0)
			{
				errno = PGTYPES_TS_BAD_TIMESTAMP;
				return (noresult);
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
			errno = PGTYPES_TS_BAD_TIMESTAMP;
			return (noresult);

		default:
			errno = PGTYPES_TS_BAD_TIMESTAMP;
			return (noresult);
	}

	/* AdjustTimestampForTypmod(&result, typmod); */

	/*
	 * Since it's difficult to test for noresult, make sure errno is 0 if no
	 * error occurred.
	 */
	errno = 0;
	return result;
}

char *
PGTYPEStimestamp_to_asc(timestamp tstamp)
{
	struct tm	tt,
			   *tm = &tt;
	char		buf[MAXDATELEN + 1];
	fsec_t		fsec;
	int			DateStyle = 1;	/* this defaults to ISO_DATES, shall we make
								 * it an option? */

	if (TIMESTAMP_NOT_FINITE(tstamp))
		EncodeSpecialTimestamp(tstamp, buf);
	else if (timestamp2tm(tstamp, NULL, tm, &fsec, NULL) == 0)
		EncodeDateTime(tm, fsec, false, 0, NULL, DateStyle, buf, 0);
	else
	{
		errno = PGTYPES_TS_BAD_TIMESTAMP;
		return NULL;
	}
	return pgtypes_strdup(buf);
}

void
PGTYPEStimestamp_current(timestamp * ts)
{
	struct tm	tm;

	GetCurrentDateTime(&tm);
	if (errno == 0)
		tm2timestamp(&tm, 0, NULL, ts);
	return;
}

static int
dttofmtasc_replace(timestamp * ts, date dDate, int dow, struct tm * tm,
				   char *output, int *pstr_len, const char *fmtstr)
{
	union un_fmt_comb replace_val;
	int			replace_type;
	int			i;
	const char *p = fmtstr;
	char	   *q = output;

	while (*p)
	{
		if (*p == '%')
		{
			p++;
			/* fix compiler warning */
			replace_type = PGTYPES_TYPE_NOTHING;
			switch (*p)
			{
					/* the abbreviated name of the day in the week */
					/* XXX should be locale aware */
				case 'a':
					replace_val.str_val = pgtypes_date_weekdays_short[dow];
					replace_type = PGTYPES_TYPE_STRING_CONSTANT;
					break;
					/* the full name of the day in the week */
					/* XXX should be locale aware */
				case 'A':
					replace_val.str_val = days[dow];
					replace_type = PGTYPES_TYPE_STRING_CONSTANT;
					break;
					/* the abbreviated name of the month */
					/* XXX should be locale aware */
				case 'b':
				case 'h':
					replace_val.str_val = months[tm->tm_mon];
					replace_type = PGTYPES_TYPE_STRING_CONSTANT;
					break;
					/* the full name of the month */
					/* XXX should be locale aware */
				case 'B':
					replace_val.str_val = pgtypes_date_months[tm->tm_mon];
					replace_type = PGTYPES_TYPE_STRING_CONSTANT;
					break;

					/*
					 * The	preferred  date  and  time	representation	for
					 * the current locale.
					 */
				case 'c':
					/* XXX */
					break;
					/* the century number with leading zeroes */
				case 'C':
					replace_val.uint_val = tm->tm_year / 100;
					replace_type = PGTYPES_TYPE_UINT_2_LZ;
					break;
					/* day with leading zeroes (01 - 31) */
				case 'd':
					replace_val.uint_val = tm->tm_mday;
					replace_type = PGTYPES_TYPE_UINT_2_LZ;
					break;
					/* the date in the format mm/dd/yy */
				case 'D':

					/*
					 * ts, dDate, dow, tm is information about the timestamp
					 *
					 * q is the start of the current output buffer
					 *
					 * pstr_len is a pointer to the remaining size of output,
					 * i.e. the size of q
					 */
					i = dttofmtasc_replace(ts, dDate, dow, tm,
										   q, pstr_len,
										   "%m/%d/%y");
					if (i)
						return i;
					break;
					/* day with leading spaces (01 - 31) */
				case 'e':
					replace_val.uint_val = tm->tm_mday;
					replace_type = PGTYPES_TYPE_UINT_2_LS;
					break;

					/*
					 * alternative format modifier
					 */
				case 'E':
					{
						char		tmp[4] = "%Ex";

						p++;
						if (*p == '\0')
							return -1;
						tmp[2] = *p;

						/*
						 * strftime's month is 0 based, ours is 1 based
						 */
						tm->tm_mon -= 1;
						i = strftime(q, *pstr_len, tmp, tm);
						if (i == 0)
							return -1;
						while (*q)
						{
							q++;
							(*pstr_len)--;
						}
						tm->tm_mon += 1;
						replace_type = PGTYPES_TYPE_NOTHING;
						break;
					}

					/*
					 * The ISO 8601 year with century as a decimal number. The
					 * 4-digit year corresponding to the ISO week number.
					 */
				case 'G':
					{
						/* Keep compiler quiet - Don't use a literal format */
						const char *fmt = "%G";

						tm->tm_mon -= 1;
						i = strftime(q, *pstr_len, fmt, tm);
						if (i == 0)
							return -1;
						while (*q)
						{
							q++;
							(*pstr_len)--;
						}
						tm->tm_mon += 1;
						replace_type = PGTYPES_TYPE_NOTHING;
					}
					break;

					/*
					 * Like %G, but without century, i.e., with a 2-digit year
					 * (00-99).
					 */
				case 'g':
					{
						const char *fmt = "%g"; /* Keep compiler quiet about
												 * 2-digit year */

						tm->tm_mon -= 1;
						i = strftime(q, *pstr_len, fmt, tm);
						if (i == 0)
							return -1;
						while (*q)
						{
							q++;
							(*pstr_len)--;
						}
						tm->tm_mon += 1;
						replace_type = PGTYPES_TYPE_NOTHING;
					}
					break;
					/* hour (24 hour clock) with leading zeroes */
				case 'H':
					replace_val.uint_val = tm->tm_hour;
					replace_type = PGTYPES_TYPE_UINT_2_LZ;
					break;
					/* hour (12 hour clock) with leading zeroes */
				case 'I':
					replace_val.uint_val = tm->tm_hour % 12;
					replace_type = PGTYPES_TYPE_UINT_2_LZ;
					break;

					/*
					 * The day of the year as a decimal number with leading
					 * zeroes. It ranges from 001 to 366.
					 */
				case 'j':
					replace_val.uint_val = tm->tm_yday;
					replace_type = PGTYPES_TYPE_UINT_3_LZ;
					break;

					/*
					 * The hour (24 hour clock). Leading zeroes will be turned
					 * into spaces.
					 */
				case 'k':
					replace_val.uint_val = tm->tm_hour;
					replace_type = PGTYPES_TYPE_UINT_2_LS;
					break;

					/*
					 * The hour (12 hour clock). Leading zeroes will be turned
					 * into spaces.
					 */
				case 'l':
					replace_val.uint_val = tm->tm_hour % 12;
					replace_type = PGTYPES_TYPE_UINT_2_LS;
					break;
					/* The month as a decimal number with a leading zero */
				case 'm':
					replace_val.uint_val = tm->tm_mon;
					replace_type = PGTYPES_TYPE_UINT_2_LZ;
					break;
					/* The minute as a decimal number with a leading zero */
				case 'M':
					replace_val.uint_val = tm->tm_min;
					replace_type = PGTYPES_TYPE_UINT_2_LZ;
					break;
					/* A newline character */
				case 'n':
					replace_val.char_val = '\n';
					replace_type = PGTYPES_TYPE_CHAR;
					break;
					/* the AM/PM specifier (uppercase) */
					/* XXX should be locale aware */
				case 'p':
					if (tm->tm_hour < 12)
						replace_val.str_val = "AM";
					else
						replace_val.str_val = "PM";
					replace_type = PGTYPES_TYPE_STRING_CONSTANT;
					break;
					/* the AM/PM specifier (lowercase) */
					/* XXX should be locale aware */
				case 'P':
					if (tm->tm_hour < 12)
						replace_val.str_val = "am";
					else
						replace_val.str_val = "pm";
					replace_type = PGTYPES_TYPE_STRING_CONSTANT;
					break;
					/* the time in the format %I:%M:%S %p */
					/* XXX should be locale aware */
				case 'r':
					i = dttofmtasc_replace(ts, dDate, dow, tm,
										   q, pstr_len,
										   "%I:%M:%S %p");
					if (i)
						return i;
					break;
					/* The time in 24 hour notation (%H:%M) */
				case 'R':
					i = dttofmtasc_replace(ts, dDate, dow, tm,
										   q, pstr_len,
										   "%H:%M");
					if (i)
						return i;
					break;
					/* The number of seconds since the Epoch (1970-01-01) */
				case 's':
#ifdef HAVE_INT64_TIMESTAMP
					replace_val.int64_val = (*ts - SetEpochTimestamp()) / 1000000.0;
					replace_type = PGTYPES_TYPE_INT64;
#else
					replace_val.double_val = *ts - SetEpochTimestamp();
					replace_type = PGTYPES_TYPE_DOUBLE_NF;
#endif
					break;
					/* seconds as a decimal number with leading zeroes */
				case 'S':
					replace_val.uint_val = tm->tm_sec;
					replace_type = PGTYPES_TYPE_UINT_2_LZ;
					break;
					/* A tabulator */
				case 't':
					replace_val.char_val = '\t';
					replace_type = PGTYPES_TYPE_CHAR;
					break;
					/* The time in 24 hour notation (%H:%M:%S) */
				case 'T':
					i = dttofmtasc_replace(ts, dDate, dow, tm,
										   q, pstr_len,
										   "%H:%M:%S");
					if (i)
						return i;
					break;

					/*
					 * The day of the week as a decimal, Monday = 1, Sunday =
					 * 7
					 */
				case 'u':
					replace_val.uint_val = dow;
					if (replace_val.uint_val == 0)
						replace_val.uint_val = 7;
					replace_type = PGTYPES_TYPE_UINT;
					break;
					/* The week number of the year as a decimal number */
				case 'U':
					tm->tm_mon -= 1;
					i = strftime(q, *pstr_len, "%U", tm);
					if (i == 0)
						return -1;
					while (*q)
					{
						q++;
						(*pstr_len)--;
					}
					tm->tm_mon += 1;
					replace_type = PGTYPES_TYPE_NOTHING;
					break;

					/*
					 * The ISO 8601:1988 week number of the current year as a
					 * decimal number.
					 */
				case 'V':
					{
						/* Keep compiler quiet - Don't use a literal format */
						const char *fmt = "%V";

						i = strftime(q, *pstr_len, fmt, tm);
						if (i == 0)
							return -1;
						while (*q)
						{
							q++;
							(*pstr_len)--;
						}
						replace_type = PGTYPES_TYPE_NOTHING;
					}
					break;

					/*
					 * The day of the week as a decimal, Sunday being 0 and
					 * Monday 1.
					 */
				case 'w':
					replace_val.uint_val = dow;
					replace_type = PGTYPES_TYPE_UINT;
					break;
					/* The week number of the year (another definition) */
				case 'W':
					tm->tm_mon -= 1;
					i = strftime(q, *pstr_len, "%U", tm);
					if (i == 0)
						return -1;
					while (*q)
					{
						q++;
						(*pstr_len)--;
					}
					tm->tm_mon += 1;
					replace_type = PGTYPES_TYPE_NOTHING;
					break;

					/*
					 * The preferred date representation for the current
					 * locale without the time.
					 */
				case 'x':
					{
						const char *fmt = "%x"; /* Keep compiler quiet about
												 * 2-digit year */

						tm->tm_mon -= 1;
						i = strftime(q, *pstr_len, fmt, tm);
						if (i == 0)
							return -1;
						while (*q)
						{
							q++;
							(*pstr_len)--;
						}
						tm->tm_mon += 1;
						replace_type = PGTYPES_TYPE_NOTHING;
					}
					break;

					/*
					 * The preferred time representation for the current
					 * locale without the date.
					 */
				case 'X':
					tm->tm_mon -= 1;
					i = strftime(q, *pstr_len, "%X", tm);
					if (i == 0)
						return -1;
					while (*q)
					{
						q++;
						(*pstr_len)--;
					}
					tm->tm_mon += 1;
					replace_type = PGTYPES_TYPE_NOTHING;
					break;
					/* The year without the century (2 digits, leading zeroes) */
				case 'y':
					replace_val.uint_val = tm->tm_year % 100;
					replace_type = PGTYPES_TYPE_UINT_2_LZ;
					break;
					/* The year with the century (4 digits) */
				case 'Y':
					replace_val.uint_val = tm->tm_year;
					replace_type = PGTYPES_TYPE_UINT;
					break;
					/* The time zone offset from GMT */
				case 'z':
					tm->tm_mon -= 1;
					i = strftime(q, *pstr_len, "%z", tm);
					if (i == 0)
						return -1;
					while (*q)
					{
						q++;
						(*pstr_len)--;
					}
					tm->tm_mon += 1;
					replace_type = PGTYPES_TYPE_NOTHING;
					break;
					/* The name or abbreviation of the time zone */
				case 'Z':
					tm->tm_mon -= 1;
					i = strftime(q, *pstr_len, "%Z", tm);
					if (i == 0)
						return -1;
					while (*q)
					{
						q++;
						(*pstr_len)--;
					}
					tm->tm_mon += 1;
					replace_type = PGTYPES_TYPE_NOTHING;
					break;
					/* A % sign */
				case '%':
					replace_val.char_val = '%';
					replace_type = PGTYPES_TYPE_CHAR;
					break;
				case '\0':
					/* fmtstr: foo%' - The string ends with a % sign */

					/*
					 * this is not compliant to the specification
					 */
					return -1;
				default:

					/*
					 * if we don't know the pattern, we just copy it
					 */
					if (*pstr_len > 1)
					{
						*q = '%';
						q++;
						(*pstr_len)--;
						if (*pstr_len > 1)
						{
							*q = *p;
							q++;
							(*pstr_len)--;
						}
						else
						{
							*q = '\0';
							return -1;
						}
						*q = '\0';
					}
					else
						return -1;
					break;
			}
			i = pgtypes_fmt_replace(replace_val, replace_type, &q, pstr_len);
			if (i)
				return i;
		}
		else
		{
			if (*pstr_len > 1)
			{
				*q = *p;
				(*pstr_len)--;
				q++;
				*q = '\0';
			}
			else
				return -1;
		}
		p++;
	}
	return 0;
}


int
PGTYPEStimestamp_fmt_asc(timestamp * ts, char *output, int str_len, const char *fmtstr)
{
	struct tm	tm;
	fsec_t		fsec;
	date		dDate;
	int			dow;

	dDate = PGTYPESdate_from_timestamp(*ts);
	dow = PGTYPESdate_dayofweek(dDate);
	timestamp2tm(*ts, NULL, &tm, &fsec, NULL);

	return dttofmtasc_replace(ts, dDate, dow, &tm, output, &str_len, fmtstr);
}

int
PGTYPEStimestamp_sub(timestamp * ts1, timestamp * ts2, interval * iv)
{
	if (TIMESTAMP_NOT_FINITE(*ts1) || TIMESTAMP_NOT_FINITE(*ts2))
		return PGTYPES_TS_ERR_EINFTIME;
	else
		iv->time = (*ts1 - *ts2);

	iv->month = 0;

	return 0;
}

int
PGTYPEStimestamp_defmt_asc(char *str, const char *fmt, timestamp * d)
{
	int			year,
				month,
				day;
	int			hour,
				minute,
				second;
	int			tz;

	int			i;
	char	   *mstr;
	char	   *mfmt;

	if (!fmt)
		fmt = "%Y-%m-%d %H:%M:%S";
	if (!fmt[0])
		return 1;

	mstr = pgtypes_strdup(str);
	mfmt = pgtypes_strdup(fmt);

	/*
	 * initialize with impossible values so that we can see if the fields
	 * where specified at all
	 */
	/* XXX ambiguity with 1 BC for year? */
	year = -1;
	month = -1;
	day = -1;
	hour = 0;
	minute = -1;
	second = -1;
	tz = 0;

	i = PGTYPEStimestamp_defmt_scan(&mstr, mfmt, d, &year, &month, &day, &hour, &minute, &second, &tz);
	free(mstr);
	free(mfmt);
	return i;
}

/*
* add an interval to a time stamp
*
*	*tout = tin + span
*
*	 returns 0 if successful
*	 returns -1 if it fails
*
*/

int
PGTYPEStimestamp_add_interval(timestamp * tin, interval * span, timestamp * tout)
{
	if (TIMESTAMP_NOT_FINITE(*tin))
		*tout = *tin;


	else
	{
		if (span->month != 0)
		{
			struct tm	tt,
					   *tm = &tt;
			fsec_t		fsec;


			if (timestamp2tm(*tin, NULL, tm, &fsec, NULL) != 0)
				return -1;
			tm->tm_mon += span->month;
			if (tm->tm_mon > MONTHS_PER_YEAR)
			{
				tm->tm_year += (tm->tm_mon - 1) / MONTHS_PER_YEAR;
				tm->tm_mon = (tm->tm_mon - 1) % MONTHS_PER_YEAR + 1;
			}
			else if (tm->tm_mon < 1)
			{
				tm->tm_year += tm->tm_mon / MONTHS_PER_YEAR - 1;
				tm->tm_mon = tm->tm_mon % MONTHS_PER_YEAR + MONTHS_PER_YEAR;
			}


			/* adjust for end of month boundary problems... */
			if (tm->tm_mday > day_tab[isleap(tm->tm_year)][tm->tm_mon - 1])
				tm->tm_mday = (day_tab[isleap(tm->tm_year)][tm->tm_mon - 1]);


			if (tm2timestamp(tm, fsec, NULL, tin) != 0)
				return -1;
		}


		*tin += span->time;
		*tout = *tin;
	}
	return 0;

}


/*
* subtract an interval from a time stamp
*
*	*tout = tin - span
*
*	 returns 0 if successful
*	 returns -1 if it fails
*
*/

int
PGTYPEStimestamp_sub_interval(timestamp * tin, interval * span, timestamp * tout)
{
	interval	tspan;

	tspan.month = -span->month;
	tspan.time = -span->time;


	return PGTYPEStimestamp_add_interval(tin, &tspan, tout);
}
