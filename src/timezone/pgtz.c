/*-------------------------------------------------------------------------
 *
 * pgtz.c
 *	  Timezone Library Integration Functions
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/timezone/pgtz.c,v 1.14 2004/05/24 02:30:29 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#define NO_REDEFINE_TIMEFUNCS

#include "postgres.h"

#include <ctype.h>

#include "miscadmin.h"
#include "pgtime.h"
#include "pgtz.h"
#include "tzfile.h"
#include "utils/elog.h"
#include "utils/guc.h"


static char tzdir[MAXPGPATH];
static int	done_tzdir = 0;

char *
pg_TZDIR(void)
{
	if (done_tzdir)
		return tzdir;

	get_share_path(my_exec_path, tzdir);
	strcat(tzdir, "/timezone");

	done_tzdir = 1;
	return tzdir;
}

/*
 * Try to determine the system timezone (as opposed to the timezone
 * set in our own library).
 */
#define T_DAY	((time_t) (60*60*24))
#define T_MONTH ((time_t) (60*60*24*31))

struct tztry
{
	char		std_zone_name[TZ_STRLEN_MAX + 1],
				dst_zone_name[TZ_STRLEN_MAX + 1];
#define MAX_TEST_TIMES 5
	int			n_test_times;
	time_t		test_times[MAX_TEST_TIMES];
};


static bool
compare_tm(struct tm *s, struct pg_tm *p)
{
	if (s->tm_sec != p->tm_sec ||
		s->tm_min != p->tm_min ||
		s->tm_hour != p->tm_hour ||
		s->tm_mday != p->tm_mday ||
		s->tm_mon != p->tm_mon ||
		s->tm_year != p->tm_year ||
		s->tm_wday != p->tm_wday ||
		s->tm_yday != p->tm_yday ||
		s->tm_isdst != p->tm_isdst)
		return false;
	return true;
}

static bool
try_timezone(char *tzname, struct tztry *tt)
{
	int			i;
	struct tm	   *systm;
	struct pg_tm   *pgtm;

	if (!pg_tzset(tzname))
		return false;			/* can't handle the TZ name at all */

	/* Check for match at all the test times */
	for (i = 0; i < tt->n_test_times; i++)
	{
		pgtm = pg_localtime(&(tt->test_times[i]));
		if (!pgtm)
			return false;		/* probably shouldn't happen */
		systm = localtime(&(tt->test_times[i]));
		if (!compare_tm(systm, pgtm))
			return false;
	}

	return true;
}

static int
get_timezone_offset(struct tm *tm)
{
#if defined(HAVE_STRUCT_TM_TM_ZONE)
	return tm->tm_gmtoff;
#elif defined(HAVE_INT_TIMEZONE)
#ifdef HAVE_UNDERSCORE_TIMEZONE
	return -_timezone;
#else
	return -timezone;
#endif
#else
#error No way to determine TZ? Can this happen?
#endif
}


#ifdef WIN32
#define TZABBREV(tz) win32_get_timezone_abbrev(tz)

static char *
win32_get_timezone_abbrev(char *tz)
{
	static char w32tzabbr[TZ_STRLEN_MAX + 1];
	int			l = 0;
	char	   *c;

	for (c = tz; *c; c++)
	{
		if (isupper(*c))
			w32tzabbr[l++] = *c;
	}
	w32tzabbr[l] = '\0';
	return w32tzabbr;
}

#else
#define TZABBREV(tz) tz
#endif


/*
 * Try to identify a timezone name (in our terminology) that matches the
 * observed behavior of the system timezone library.  We cannot assume that
 * the system TZ environment setting (if indeed there is one) matches our
 * terminology, so we ignore it and just look at what localtime() returns.
 */
static char *
identify_system_timezone(void)
{
	static char resultbuf[TZ_STRLEN_MAX + 1];
	time_t		tnow;
	time_t		t;
	int			nowisdst,
				curisdst;
	int			std_ofs = 0;
	struct tztry tt;
	struct tm  *tm;
	char		cbuf[TZ_STRLEN_MAX + 1];

	/* Initialize OS timezone library */
	tzset();

	/* No info yet */
	memset(&tt, 0, sizeof(tt));

	/*
	 * The idea here is to scan forward from today and try to locate the
	 * next two daylight-savings transition boundaries.  We will test for
	 * correct results on the day before and after each boundary; this
	 * gives at least some confidence that we've selected the right DST
	 * rule set.
	 */
	tnow = time(NULL);

	/*
	 * Round back to a GMT midnight so results don't depend on local time
	 * of day
	 */
	tnow -= (tnow % T_DAY);

	/* Always test today, so we have at least one test point */
	tt.test_times[tt.n_test_times++] = tnow;

	tm = localtime(&tnow);
	nowisdst = tm->tm_isdst;
	curisdst = nowisdst;

	if (curisdst == 0)
	{
		/* Set up STD zone name, in case we are in a non-DST zone */
		memset(cbuf, 0, sizeof(cbuf));
		strftime(cbuf, sizeof(cbuf) - 1, "%Z", tm); /* zone abbr */
		strcpy(tt.std_zone_name, TZABBREV(cbuf));
		/* Also preset std_ofs */
		std_ofs = get_timezone_offset(tm);
	}

	/*
	 * We have to look a little further ahead than one year, in case today
	 * is just past a DST boundary that falls earlier in the year than the
	 * next similar boundary.  Arbitrarily scan up to 14 months.
	 */
	for (t = tnow + T_DAY; t < tnow + T_MONTH * 14; t += T_DAY)
	{
		tm = localtime(&t);
		if (tm->tm_isdst >= 0 && tm->tm_isdst != curisdst)
		{
			/* Found a boundary */
			tt.test_times[tt.n_test_times++] = t - T_DAY;
			tt.test_times[tt.n_test_times++] = t;
			curisdst = tm->tm_isdst;
			/* Save STD or DST zone name, also std_ofs */
			memset(cbuf, 0, sizeof(cbuf));
			strftime(cbuf, sizeof(cbuf) - 1, "%Z", tm); /* zone abbr */
			if (curisdst == 0)
			{
				strcpy(tt.std_zone_name, TZABBREV(cbuf));
				std_ofs = get_timezone_offset(tm);
			}
			else
				strcpy(tt.dst_zone_name, TZABBREV(cbuf));
			/* Have we found two boundaries? */
			if (tt.n_test_times >= 5)
				break;
		}
	}

	/* We should have found a STD zone name by now... */
	if (tt.std_zone_name[0] == '\0')
	{
		ereport(LOG,
				(errmsg("unable to determine system timezone, defaulting to \"%s\"", "GMT"),
				 errhint("You can specify the correct timezone in postgresql.conf.")));
		return NULL;			/* go to GMT */
	}

	/* If we found DST too then try STD<ofs>DST */
	if (tt.dst_zone_name[0] != '\0')
	{
		snprintf(resultbuf, sizeof(resultbuf), "%s%d%s",
				 tt.std_zone_name, -std_ofs / 3600, tt.dst_zone_name);
		if (try_timezone(resultbuf, &tt))
			return resultbuf;
	}

	/* Try just the STD timezone (works for GMT at least) */
	strcpy(resultbuf, tt.std_zone_name);
	if (try_timezone(resultbuf, &tt))
		return resultbuf;

	/* Try STD<ofs> */
	snprintf(resultbuf, sizeof(resultbuf), "%s%d",
			 tt.std_zone_name, -std_ofs / 3600);
	if (try_timezone(resultbuf, &tt))
		return resultbuf;

	/*
	 * Did not find the timezone.  Fallback to use a GMT zone.  Note that the
	 * zic timezone database names the GMT-offset zones in POSIX style: plus
	 * is west of Greenwich.  It's unfortunate that this is opposite of SQL
	 * conventions.  Should we therefore change the names?  Probably not...
	 */
	snprintf(resultbuf, sizeof(resultbuf), "Etc/GMT%s%d",
			(-std_ofs > 0) ? "+" : "", -std_ofs / 3600);

	ereport(LOG,
			(errmsg("could not recognize system timezone, defaulting to \"%s\"",
					resultbuf),
			 errhint("You can specify the correct timezone in postgresql.conf.")));
	return resultbuf;
}

/*
 * Check whether timezone is acceptable.
 *
 * What we are doing here is checking for leap-second-aware timekeeping.
 * We need to reject such TZ settings because they'll wreak havoc with our
 * date/time arithmetic.
 *
 * NB: this must NOT ereport(ERROR).  The caller must get control back so that
 * it can restore the old value of TZ if we don't like the new one.
 */
bool
tz_acceptable(void)
{
	struct pg_tm tt;
	time_t		time2000;

	/*
	 * To detect leap-second timekeeping, compute the time_t value for
	 * local midnight, 2000-01-01.	Insist that this be a multiple of 60;
	 * any partial-minute offset has to be due to leap seconds.
	 */
	MemSet(&tt, 0, sizeof(tt));
	tt.tm_year = 100;
	tt.tm_mon = 0;
	tt.tm_mday = 1;
	tt.tm_isdst = -1;
	time2000 = pg_mktime(&tt);
	if ((time2000 % 60) != 0)
		return false;

	return true;
}

/*
 * Identify a suitable default timezone setting based on the environment,
 * and make it active.
 *
 * We first look to the TZ environment variable.  If not found or not
 * recognized by our own code, we see if we can identify the timezone
 * from the behavior of the system timezone library.  When all else fails,
 * fall back to GMT.
 */
const char *
select_default_timezone(void)
{
	char	   *def_tz;

	def_tz = getenv("TZ");
	if (def_tz && pg_tzset(def_tz) && tz_acceptable())
		return def_tz;

	def_tz = identify_system_timezone();
	if (def_tz && pg_tzset(def_tz) && tz_acceptable())
		return def_tz;

	if (pg_tzset("GMT") && tz_acceptable())
		return "GMT";

	ereport(FATAL,
			(errmsg("could not select a suitable default timezone"),
			 errdetail("It appears that your GMT time zone uses leap seconds. PostgreSQL does not support leap seconds.")));
	return NULL;				/* keep compiler quiet */
}

/*
 * Initialize timezone library
 *
 * This is called after initial loading of postgresql.conf.  If no TimeZone
 * setting was found therein, we try to derive one from the environment.
 */
void
pg_timezone_initialize(void)
{
	/* Do we need to try to figure the timezone? */
	if (pg_strcasecmp(GetConfigOption("timezone"), "UNKNOWN") == 0)
	{
		const char *def_tz;

		/* Select setting */
		def_tz = select_default_timezone();
		/* Tell GUC about the value. Will redundantly call pg_tzset() */
		SetConfigOption("timezone", def_tz, PGC_POSTMASTER, PGC_S_ENV_VAR);
	}
}
