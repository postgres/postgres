/*-------------------------------------------------------------------------
 *
 * pgtz.c
 *	  Timezone Library Integration Functions
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/timezone/pgtz.c,v 1.20 2004/07/30 17:31:24 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#define NO_REDEFINE_TIMEFUNCS

#include "postgres.h"

#include <ctype.h>
#include <sys/stat.h>
#include <time.h>

#include "miscadmin.h"
#include "pgtime.h"
#include "pgtz.h"
#include "storage/fd.h"
#include "tzfile.h"
#include "utils/datetime.h"
#include "utils/elog.h"
#include "utils/guc.h"


#define T_DAY	((time_t) (60*60*24))
#define T_WEEK  ((time_t) (60*60*24*7))
#define T_MONTH ((time_t) (60*60*24*31))

#define MAX_TEST_TIMES (52*40)	/* 40 years, or 1964..2004 */

struct tztry
{
	int			n_test_times;
	time_t		test_times[MAX_TEST_TIMES];
};

static char tzdir[MAXPGPATH];
static int	done_tzdir = 0;

static void scan_available_timezones(char *tzdir, char *tzdirsub,
									 struct tztry *tt,
									 int *bestscore, char *bestzonename);


/*
 * Return full pathname of timezone data directory
 */
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
 * Get GMT offset from a system struct tm
 */
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

/*
 * Grotty kluge for win32 ... do we really need this?
 */
#ifdef WIN32
#define TZABBREV(tz) win32_get_timezone_abbrev(tz)

static char *
win32_get_timezone_abbrev(const char *tz)
{
	static char w32tzabbr[TZ_STRLEN_MAX + 1];
	int			l = 0;
	const char  *c;

	for (c = tz; *c; c++)
	{
		if (isupper((unsigned char) *c))
			w32tzabbr[l++] = *c;
	}
	w32tzabbr[l] = '\0';
	return w32tzabbr;
}

#else
#define TZABBREV(tz) (tz)
#endif

/*
 * Convenience subroutine to convert y/m/d to time_t (NOT pg_time_t)
 */
static time_t
build_time_t(int year, int month, int day)
{
	struct tm	tm;

	memset(&tm, 0, sizeof(tm));
	tm.tm_mday = day;
	tm.tm_mon = month - 1;
	tm.tm_year = year - 1900;

	return mktime(&tm);
}

/*
 * Does a system tm value match one we computed ourselves?
 */
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

/*
 * See how well a specific timezone setting matches the system behavior
 *
 * We score a timezone setting according to the number of test times it
 * matches.  (The test times are ordered later-to-earlier, but this routine
 * doesn't actually know that; it just scans until the first non-match.)
 *
 * We return -1 for a completely unusable setting; this is worse than the
 * score of zero for a setting that works but matches not even the first
 * test time.
 */
static int
score_timezone(const char *tzname, struct tztry *tt)
{
	int			i;
	pg_time_t	pgtt;
	struct tm	   *systm;
	struct pg_tm   *pgtm;
	char		cbuf[TZ_STRLEN_MAX + 1];

	if (!pg_tzset(tzname))
		return -1;				/* can't handle the TZ name at all */

	/* Reject if leap seconds involved */
	if (!tz_acceptable())
	{
		elog(DEBUG4, "Reject TZ \"%s\": uses leap seconds", tzname);
		return -1;
	}

	/* Check for match at all the test times */
	for (i = 0; i < tt->n_test_times; i++)
	{
		pgtt = (pg_time_t) (tt->test_times[i]);
		pgtm = pg_localtime(&pgtt);
		if (!pgtm)
			return -1;		/* probably shouldn't happen */
		systm = localtime(&(tt->test_times[i]));
		if (!systm)
		{
			elog(DEBUG4, "TZ \"%s\" scores %d: at %ld %04d-%02d-%02d %02d:%02d:%02d %s, system had no data",
				 tzname, i, (long) pgtt,
				 pgtm->tm_year + 1900, pgtm->tm_mon + 1, pgtm->tm_mday,
				 pgtm->tm_hour, pgtm->tm_min, pgtm->tm_sec,
				 pgtm->tm_isdst ? "dst" : "std");
			return i;
		}
		if (!compare_tm(systm, pgtm))
		{
			elog(DEBUG4, "TZ \"%s\" scores %d: at %ld %04d-%02d-%02d %02d:%02d:%02d %s versus %04d-%02d-%02d %02d:%02d:%02d %s",
				 tzname, i, (long) pgtt,
				 pgtm->tm_year + 1900, pgtm->tm_mon + 1, pgtm->tm_mday,
				 pgtm->tm_hour, pgtm->tm_min, pgtm->tm_sec,
				 pgtm->tm_isdst ? "dst" : "std",
				 systm->tm_year + 1900, systm->tm_mon + 1, systm->tm_mday,
				 systm->tm_hour, systm->tm_min, systm->tm_sec,
				 systm->tm_isdst ? "dst" : "std");
			return i;
		}
		if (systm->tm_isdst >= 0)
		{
			/* Check match of zone names, too */
			if (pgtm->tm_zone == NULL)
				return -1;		/* probably shouldn't happen */
			memset(cbuf, 0, sizeof(cbuf));
			strftime(cbuf, sizeof(cbuf) - 1, "%Z", systm); /* zone abbr */
			if (strcmp(TZABBREV(cbuf), pgtm->tm_zone) != 0)
			{
				elog(DEBUG4, "TZ \"%s\" scores %d: at %ld \"%s\" versus \"%s\"",
					 tzname, i, (long) pgtt,
					 pgtm->tm_zone, cbuf);
				return i;
			}
		}
	}

	elog(DEBUG4, "TZ \"%s\" gets max score %d", tzname, i);
	return i;
}


/*
 * Try to identify a timezone name (in our terminology) that best matches the
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
	struct tztry tt;
	struct tm  *tm;
	int			bestscore;
	char		tmptzdir[MAXPGPATH];
	int			std_ofs;
	char		std_zone_name[TZ_STRLEN_MAX + 1],
				dst_zone_name[TZ_STRLEN_MAX + 1];
	char		cbuf[TZ_STRLEN_MAX + 1];

	/* Initialize OS timezone library */
	tzset();

	/*
	 * Set up the list of dates to be probed to see how well our timezone
	 * matches the system zone.  We first probe January and July of 2004;
	 * this serves to quickly eliminate the vast majority of the TZ database
	 * entries.  If those dates match, we probe every week from 2004 backwards
	 * to late 1964.  (Weekly resolution is good enough to identify DST
	 * transition rules, since everybody switches on Sundays.)  The further
	 * back the zone matches, the better we score it.  This may seem like
	 * a rather random way of doing things, but experience has shown that
	 * system-supplied timezone definitions are likely to have DST behavior
	 * that is right for the recent past and not so accurate further back.
	 * Scoring in this way allows us to recognize zones that have some
	 * commonality with the zic database, without insisting on exact match.
	 * (Note: we probe Thursdays, not Sundays, to avoid triggering
	 * DST-transition bugs in localtime itself.)
	 */
	tt.n_test_times = 0;
	tt.test_times[tt.n_test_times++] = build_time_t(2004, 1, 15);
	tt.test_times[tt.n_test_times++] = t = build_time_t(2004, 7, 15);
	while (tt.n_test_times < MAX_TEST_TIMES)
	{
		t -= T_WEEK;
		tt.test_times[tt.n_test_times++] = t;
	}

	/* Search for the best-matching timezone file */
	strcpy(tmptzdir, pg_TZDIR());
	bestscore = 0;
	scan_available_timezones(tmptzdir, tmptzdir + strlen(tmptzdir) + 1,
							 &tt,
							 &bestscore, resultbuf);
	if (bestscore > 0)
		return resultbuf;

	/*
	 * Couldn't find a match in the database, so next we try constructed zone
	 * names (like "PST8PDT").
	 *
	 * First we need to determine the names of the local standard and daylight
	 * zones.  The idea here is to scan forward from today until we have
	 * seen both zones, if both are in use.
	 */
	memset(std_zone_name, 0, sizeof(std_zone_name));
	memset(dst_zone_name, 0, sizeof(dst_zone_name));
	std_ofs = 0;

	tnow = time(NULL);

	/*
	 * Round back to a GMT midnight so results don't depend on local time
	 * of day
	 */
	tnow -= (tnow % T_DAY);

	/*
	 * We have to look a little further ahead than one year, in case today
	 * is just past a DST boundary that falls earlier in the year than the
	 * next similar boundary.  Arbitrarily scan up to 14 months.
	 */
	for (t = tnow; t <= tnow + T_MONTH * 14; t += T_MONTH)
	{
		tm = localtime(&t);
		if (!tm)
			continue;
		if (tm->tm_isdst < 0)
			continue;
		if (tm->tm_isdst == 0 && std_zone_name[0] == '\0')
		{
			/* found STD zone */
			memset(cbuf, 0, sizeof(cbuf));
			strftime(cbuf, sizeof(cbuf) - 1, "%Z", tm); /* zone abbr */
			strcpy(std_zone_name, TZABBREV(cbuf));
			std_ofs = get_timezone_offset(tm);
		}
		if (tm->tm_isdst > 0 && dst_zone_name[0] == '\0')
		{
			/* found DST zone */
			memset(cbuf, 0, sizeof(cbuf));
			strftime(cbuf, sizeof(cbuf) - 1, "%Z", tm); /* zone abbr */
			strcpy(dst_zone_name, TZABBREV(cbuf));
		}
		/* Done if found both */
		if (std_zone_name[0] && dst_zone_name[0])
			break;
	}

	/* We should have found a STD zone name by now... */
	if (std_zone_name[0] == '\0')
	{
		ereport(LOG,
				(errmsg("unable to determine system timezone, defaulting to \"%s\"", "GMT"),
				 errhint("You can specify the correct timezone in postgresql.conf.")));
		return NULL;			/* go to GMT */
	}

	/* If we found DST then try STD<ofs>DST */
	if (dst_zone_name[0] != '\0')
	{
		snprintf(resultbuf, sizeof(resultbuf), "%s%d%s",
				 std_zone_name, -std_ofs / 3600, dst_zone_name);
		if (score_timezone(resultbuf, &tt) > 0)
			return resultbuf;
	}

	/* Try just the STD timezone (works for GMT at least) */
	strcpy(resultbuf, std_zone_name);
	if (score_timezone(resultbuf, &tt) > 0)
		return resultbuf;

	/* Try STD<ofs> */
	snprintf(resultbuf, sizeof(resultbuf), "%s%d",
			 std_zone_name, -std_ofs / 3600);
	if (score_timezone(resultbuf, &tt) > 0)
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
 * Recursively scan the timezone database looking for the best match to
 * the system timezone behavior.
 *
 * tzdir points to a buffer of size MAXPGPATH.  On entry, it holds the
 * pathname of a directory containing TZ files.  We internally modify it
 * to hold pathnames of sub-directories and files, but must restore it
 * to its original contents before exit.
 *
 * tzdirsub points to the part of tzdir that represents the subfile name
 * (ie, tzdir + the original directory name length, plus one for the
 * first added '/').
 *
 * tt tells about the system timezone behavior we need to match.
 *
 * *bestscore and *bestzonename on entry hold the best score found so far
 * and the name of the best zone.  We overwrite them if we find a better
 * score.  bestzonename must be a buffer of length TZ_STRLEN_MAX + 1.
 */
static void
scan_available_timezones(char *tzdir, char *tzdirsub, struct tztry *tt,
						 int *bestscore, char *bestzonename)
{
	int			tzdir_orig_len = strlen(tzdir);
	DIR		   *dirdesc;

	dirdesc = AllocateDir(tzdir);
	if (!dirdesc)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m", tzdir)));
		return;
	}

	for (;;)
	{
		struct dirent *direntry;
		struct stat statbuf;

		errno = 0;
		direntry = readdir(dirdesc);
		if (!direntry)
		{
#ifdef WIN32
			/* This fix is in mingw cvs (runtime/mingwex/dirent.c rev 1.4),
			 * but not in released version
			 */
			if (GetLastError() == ERROR_NO_MORE_FILES)
				errno = 0;
#endif
			if (errno)
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("error reading directory: %m")));
			break;
		}

		/* Ignore . and .., plus any other "hidden" files */
		if (direntry->d_name[0] == '.')
			continue;

		snprintf(tzdir + tzdir_orig_len, MAXPGPATH - tzdir_orig_len,
				 "/%s", direntry->d_name);

		if (stat(tzdir, &statbuf) != 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not stat \"%s\": %m", tzdir)));
			continue;
		}

		if (S_ISDIR(statbuf.st_mode))
		{
			/* Recurse into subdirectory */
			scan_available_timezones(tzdir, tzdirsub, tt,
									 bestscore, bestzonename);
		}
		else
		{
			/* Load and test this file */
			int score = score_timezone(tzdirsub, tt);

			if (score > *bestscore)
			{
				*bestscore = score;
				StrNCpy(bestzonename, tzdirsub, TZ_STRLEN_MAX + 1);
			}
		}
	}

	FreeDir(dirdesc);

	/* Restore tzdir */
	tzdir[tzdir_orig_len] = '\0';
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
	struct pg_tm *tt;
	pg_time_t	time2000;

	/*
	 * To detect leap-second timekeeping, run pg_localtime for what should
	 * be GMT midnight, 2000-01-01.  Insist that the tm_sec value be zero;
	 * any other result has to be due to leap seconds.
	 */
	time2000 = (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400;
	tt = pg_localtime(&time2000);
	if (!tt || tt->tm_sec != 0)
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
		SetConfigOption("timezone", def_tz, PGC_POSTMASTER, PGC_S_ARGV);
	}
}
