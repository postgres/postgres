#include "postgres_fe.h"
#include <time.h>
#include <float.h>
#include <math.h>

#ifdef __FAST_MATH__
#error -ffast-math is known to break this code
#endif

#include "extern.h"
#include "dt.h"
#include "pgtypes_timestamp.h"
#include "pgtypes_date.h"

int PGTYPEStimestamp_defmt_scan(char **, char *, timestamp *, int *, int *, int *,
							int *, int *, int *, int *);

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

static timestamp
dt2local(timestamp dt, int tz)
{
#ifdef HAVE_INT64_TIMESTAMP
	dt -= (tz * INT64CONST(1000000));
#else
	dt -= tz;
	dt = JROUND(dt);
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
tm2timestamp(struct tm * tm, fsec_t fsec, int *tzp, timestamp *result)
{
#ifdef HAVE_INT64_TIMESTAMP
	int		dDate;
	int64		time;

#else
	double		dDate, time;
#endif

	/* Julian day routines are not correct for negative Julian days */
	if (!IS_VALID_JULIAN(tm->tm_year, tm->tm_mon, tm->tm_mday))
		return -1;

	dDate = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - date2j(2000, 1, 1);
	time = time2t(tm->tm_hour, tm->tm_min, tm->tm_sec, fsec);
#ifdef HAVE_INT64_TIMESTAMP
	*result = (dDate * INT64CONST(86400000000)) + time;
	/* check for major overflow */
	if ((*result - time) / INT64CONST(86400000000) != dDate)
		return -1;
	/* check for just-barely overflow (okay except time-of-day wraps) */
	if ((*result < 0) ? (dDate >= 0) : (dDate < 0))
		return -1;
#else
	*result = ((dDate * 86400) + time);
#endif
	if (tzp != NULL)
		*result = dt2local(*result, -(*tzp));

	return 0;
}	/* tm2timestamp() */

static timestamp
SetEpochTimestamp(void)
{
	timestamp	dt;
	struct tm	tt,
			   *tm = &tt;

	GetEpochTime(tm);
	tm2timestamp(tm, 0, NULL, &dt);
	return dt;
}	/* SetEpochTimestamp() */

static void
dt2time(timestamp jd, int *hour, int *min, int *sec, fsec_t *fsec)
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
static int
timestamp2tm(timestamp dt, int *tzp, struct tm * tm, fsec_t *fsec, char **tzn)
{
#ifdef HAVE_INT64_TIMESTAMP
	int		dDate, date0;
	int64		time;

#else
	double		dDate, date0;
	double		time;
#endif
	time_t		utime;

#if defined(HAVE_TM_ZONE) || defined(HAVE_INT_TIMEZONE)
	struct tm  *tx;
#endif

	date0 = date2j(2000, 1, 1);

	time = dt;
#ifdef HAVE_INT64_TIMESTAMP
	TMODULO(time, dDate, INT64CONST(86400000000));

	if (time < INT64CONST(0))
	{
		time += INT64CONST(86400000000);
		dDate -= 1;
	}
#else
	TMODULO(time, dDate, 86400e0);

	if (time < 0)
	{
		time += 86400;
		dDate -= 1;
	}
#endif

	/* Julian day routine does not work for negative Julian days */
	if (dDate < -date0)
		return -1;

	/* add offset to go from J2000 back to standard Julian date */
	dDate += date0;

	j2date((int) dDate, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
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
	int			tz;
	int			dtype;
	int			nf;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		lowstr[MAXDATELEN + MAXDATEFIELDS];
	char	   *realptr;
	char	  **ptr = (endptr != NULL) ? endptr : &realptr;

	if (strlen(str) >= sizeof(lowstr))
	{
		errno = PGTYPES_TS_BAD_TIMESTAMP;
		return (noresult);
	}

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf, ptr) != 0)
	|| (DecodeDateTime(field, ftype, nf, &dtype, tm, &fsec, &tz, 0) != 0))
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

	/* Since it's difficult to test for noresult, make sure errno is 0 if no error occured. */
	errno = 0;
	return result;
}

char *
PGTYPEStimestamp_to_asc(timestamp tstamp)
{
	struct tm	tt,
			   *tm = &tt;
	char		buf[MAXDATELEN + 1];
	char	   *tzn = NULL;
	fsec_t		fsec;
	int			DateStyle = 1;	/* this defaults to ISO_DATES, shall we
								 * make it an option? */

	if (TIMESTAMP_NOT_FINITE(tstamp))
		EncodeSpecialTimestamp(tstamp, buf);
	else if (timestamp2tm(tstamp, NULL, tm, &fsec, NULL) == 0)
		EncodeDateTime(tm, fsec, NULL, &tzn, DateStyle, buf, 0);
	else
	{
		errno = PGTYPES_TS_BAD_TIMESTAMP;
		return NULL;
	}
	return pgtypes_strdup(buf);
}

void
PGTYPEStimestamp_current(timestamp *ts)
{
	struct tm	tm;

	GetCurrentDateTime(&tm);
	tm2timestamp(&tm, 0, NULL, ts);
	return;
}

static int
dttofmtasc_replace(timestamp *ts, date dDate, int dow, struct tm * tm,
				   char *output, int *pstr_len, char *fmtstr)
{
	union un_fmt_comb replace_val;
	int			replace_type;
	int			i;
	char	   *p = fmtstr;
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
				case 'a':
					replace_val.str_val = pgtypes_date_weekdays_short[dow];
					replace_type = PGTYPES_TYPE_STRING_CONSTANT;
					break;
				case 'A':
					replace_val.str_val = days[dow];
					replace_type = PGTYPES_TYPE_STRING_CONSTANT;
					break;
				case 'b':
				case 'h':
					replace_val.str_val = months[tm->tm_mon];
					replace_type = PGTYPES_TYPE_STRING_CONSTANT;
					break;
				case 'B':
					replace_val.str_val = pgtypes_date_months[tm->tm_mon];
					replace_type = PGTYPES_TYPE_STRING_CONSTANT;
					break;
				case 'c':
					/* XXX */
					break;
				case 'C':
					replace_val.uint_val = tm->tm_year / 100;
					replace_type = PGTYPES_TYPE_UINT_2_LZ;
					break;
				case 'd':
					replace_val.uint_val = tm->tm_mday;
					replace_type = PGTYPES_TYPE_UINT_2_LZ;
					break;
				case 'D':

					/*
					 * ts, dDate, dow, tm is information about the
					 * timestamp
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
				case 'e':
					replace_val.uint_val = tm->tm_mday;
					replace_type = PGTYPES_TYPE_UINT_2_LS;
					break;
				case 'E':
					{
						char		tmp[4] = "%Ex";

						p++;
						if (*p == '\0')
							return -1;
						tmp[2] = *p;
						/* XXX: fall back to strftime */

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
				case 'G':
					/* XXX: fall back to strftime */
					tm->tm_mon -= 1;
					i = strftime(q, *pstr_len, "%G", tm);
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
				case 'g':
					/* XXX: fall back to strftime */
					{
						char	   *fmt = "%g"; /* Keep compiler quiet
												 * about 2-digit year */

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
				case 'H':
					replace_val.uint_val = tm->tm_hour;
					replace_type = PGTYPES_TYPE_UINT_2_LZ;
					break;
				case 'I':
					replace_val.uint_val = tm->tm_hour % 12;
					replace_type = PGTYPES_TYPE_UINT_2_LZ;
					break;
				case 'j':
					replace_val.uint_val = tm->tm_yday;
					replace_type = PGTYPES_TYPE_UINT_3_LZ;
					break;
				case 'k':
					replace_val.uint_val = tm->tm_hour;
					replace_type = PGTYPES_TYPE_UINT_2_LS;
					break;
				case 'l':
					replace_val.uint_val = tm->tm_hour % 12;
					replace_type = PGTYPES_TYPE_UINT_2_LS;
					break;
				case 'm':
					replace_val.uint_val = tm->tm_mon;
					replace_type = PGTYPES_TYPE_UINT_2_LZ;
					break;
				case 'M':
					replace_val.uint_val = tm->tm_min;
					replace_type = PGTYPES_TYPE_UINT_2_LZ;
					break;
				case 'n':
					replace_val.char_val = '\n';
					replace_type = PGTYPES_TYPE_CHAR;
					break;
				case 'p':
					if (tm->tm_hour < 12)
						replace_val.str_val = "AM";
					else
						replace_val.str_val = "PM";
					replace_type = PGTYPES_TYPE_STRING_CONSTANT;
					break;
				case 'P':
					if (tm->tm_hour < 12)
						replace_val.str_val = "am";
					else
						replace_val.str_val = "pm";
					replace_type = PGTYPES_TYPE_STRING_CONSTANT;
					break;
				case 'r':
					i = dttofmtasc_replace(ts, dDate, dow, tm,
										   q, pstr_len,
										   "%I:%M:%S %p");
					if (i)
						return i;
					break;
				case 'R':
					i = dttofmtasc_replace(ts, dDate, dow, tm,
										   q, pstr_len,
										   "%H:%M");
					if (i)
						return i;
					break;
				case 's':
#ifdef HAVE_INT64_TIMESTAMP
					replace_val.int64_val = ((*ts - SetEpochTimestamp()) / 1000000e0);
					replace_type = PGTYPES_TYPE_INT64;
#else
					replace_val.double_val = *ts - SetEpochTimestamp();
					replace_type = PGTYPES_TYPE_DOUBLE_NF;
#endif
					break;
				case 'S':
					replace_val.uint_val = tm->tm_sec;
					replace_type = PGTYPES_TYPE_UINT_2_LZ;
					break;
				case 't':
					replace_val.char_val = '\t';
					replace_type = PGTYPES_TYPE_CHAR;
					break;
				case 'T':
					i = dttofmtasc_replace(ts, dDate, dow, tm,
										   q, pstr_len,
										   "%H:%M:%S");
					if (i)
						return i;
					break;
				case 'u':
					if (dow == 0)
						dow = 7;
					replace_val.uint_val = dow;
					replace_type = PGTYPES_TYPE_UINT;
					break;
				case 'U':
					/* XXX: fall back to strftime */
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
				case 'V':
					/* XXX: fall back to strftime */
					i = strftime(q, *pstr_len, "%V", tm);
					if (i == 0)
						return -1;
					while (*q)
					{
						q++;
						(*pstr_len)--;
					}
					replace_type = PGTYPES_TYPE_NOTHING;
					break;
				case 'w':
					replace_val.uint_val = dow;
					replace_type = PGTYPES_TYPE_UINT;
					break;
				case 'W':
					/* XXX: fall back to strftime */
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
				case 'x':
					/* XXX: fall back to strftime */
					{
						char	   *fmt = "%x"; /* Keep compiler quiet
												 * about 2-digit year */

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
				case 'X':
					/* XXX: fall back to strftime */
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
				case 'y':
					replace_val.uint_val = tm->tm_year % 100;
					replace_type = PGTYPES_TYPE_UINT_2_LZ;
					break;
				case 'Y':
					replace_val.uint_val = tm->tm_year;
					replace_type = PGTYPES_TYPE_UINT;
					break;
				case 'z':
					/* XXX: fall back to strftime */
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
				case 'Z':
					/* XXX: fall back to strftime */
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
				case '%':
					replace_val.char_val = '%';
					replace_type = PGTYPES_TYPE_CHAR;
					break;
				case '\0':
					/* fmtstr: blabla%' */

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
PGTYPEStimestamp_fmt_asc(timestamp *ts, char *output, int str_len, char *fmtstr)
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
PGTYPEStimestamp_sub(timestamp *ts1, timestamp *ts2, interval *iv)
{
	if (TIMESTAMP_NOT_FINITE(*ts1) || TIMESTAMP_NOT_FINITE(*ts2))
		return PGTYPES_TS_ERR_EINFTIME;
	else
#ifdef HAVE_INT64_TIMESTAMP
		iv->time = (ts1 - ts2);
#else
		iv->time = JROUND(ts1 - ts2);
#endif

	iv->month = 0;

	return 0;
}

int
PGTYPEStimestamp_defmt_asc(char *str, char *fmt, timestamp *d)
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
