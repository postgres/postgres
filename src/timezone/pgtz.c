/*-------------------------------------------------------------------------
 *
 * pgtz.c
 *	  Timezone Library Integration Functions
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/timezone/pgtz.c,v 1.12 2004/05/23 22:24:08 tgl Exp $
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
#define T_YEAR	(60*60*24*365)
#define T_MONTH (60*60*24*30)

struct tztry
{
	time_t		std_t,
				dst_t;
	char		std_time[TZ_STRLEN_MAX + 1],
				dst_time[TZ_STRLEN_MAX + 1];
	int			std_ofs,
				dst_ofs;
	struct tm	std_tm,
				dst_tm;
};


static bool
compare_tm(struct tm * s, struct pg_tm * p)
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
try_timezone(char *tzname, struct tztry * tt, bool checkdst)
{
	struct pg_tm *pgtm;

	if (!pg_tzset(tzname))
		return false;			/* If this timezone couldn't be picked at
								 * all */

	/* Verify standard time */
	pgtm = pg_localtime(&(tt->std_t));
	if (!pgtm)
		return false;
	if (!compare_tm(&(tt->std_tm), pgtm))
		return false;

	if (!checkdst)
		return true;

	/* Now check daylight time */
	pgtm = pg_localtime(&(tt->dst_t));
	if (!pgtm)
		return false;
	if (!compare_tm(&(tt->dst_tm), pgtm))
		return false;

	return true;
}

static int
get_timezone_offset(struct tm * tm)
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
 * terminology, so ignore it and just look at what localtime() returns.
 */
static char *
identify_system_timezone(void)
{
	static char __tzbuf[TZ_STRLEN_MAX + 1];
	bool		std_found = false,
				dst_found = false;
	time_t		tnow = time(NULL);
	time_t		t;
	struct tztry tt;
	char		cbuf[TZ_STRLEN_MAX + 1];

	/* Initialize OS timezone library */
	tzset();

	memset(&tt, 0, sizeof(tt));

	for (t = tnow; t < tnow + T_YEAR; t += T_MONTH)
	{
		struct tm  *tm = localtime(&t);

		if (tm->tm_isdst == 0 && !std_found)
		{
			/* Standard time */
			memcpy(&tt.std_tm, tm, sizeof(struct tm));
			memset(cbuf, 0, sizeof(cbuf));
			strftime(cbuf, sizeof(cbuf) - 1, "%Z", tm); /* zone abbr */
			strcpy(tt.std_time, TZABBREV(cbuf));
			tt.std_ofs = get_timezone_offset(tm);
			tt.std_t = t;
			std_found = true;
		}
		else if (tm->tm_isdst == 1 && !dst_found)
		{
			/* Daylight time */
			memcpy(&tt.dst_tm, tm, sizeof(struct tm));
			memset(cbuf, 0, sizeof(cbuf));
			strftime(cbuf, sizeof(cbuf) - 1, "%Z", tm); /* zone abbr */
			strcpy(tt.dst_time, TZABBREV(cbuf));
			tt.dst_ofs = get_timezone_offset(tm);
			tt.dst_t = t;
			dst_found = true;
		}
		if (std_found && dst_found)
			break;				/* Got both standard and daylight */
	}

	if (!std_found)
	{
		/* Failed to determine TZ! */
		ereport(LOG,
				(errmsg("unable to determine system timezone, defaulting to \"%s\"", "GMT"),
				 errhint("You can specify the correct timezone in postgresql.conf.")));
		return NULL;			/* go to GMT */
	}

	if (dst_found)
	{
		/* Try STD<ofs>DST */
		sprintf(__tzbuf, "%s%d%s", tt.std_time, -tt.std_ofs / 3600, tt.dst_time);
		if (try_timezone(__tzbuf, &tt, dst_found))
			return __tzbuf;
	}
	/* Try just the STD timezone */
	strcpy(__tzbuf, tt.std_time);
	if (try_timezone(__tzbuf, &tt, dst_found))
		return __tzbuf;

	/* Did not find the timezone. Fallback to try a GMT zone. */
	sprintf(__tzbuf, "Etc/GMT%s%d",
			(-tt.std_ofs < 0) ? "+" : "", tt.std_ofs / 3600);
	ereport(LOG,
	 (errmsg("could not recognize system timezone, defaulting to \"%s\"",
			 __tzbuf),
	errhint("You can specify the correct timezone in postgresql.conf.")));
	return __tzbuf;
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
