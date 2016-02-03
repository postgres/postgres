/*-------------------------------------------------------------------------
 *
 * findtimezone.c
 *	  Functions for determining the default timezone to use.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/bin/initdb/findtimezone.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

#include "pgtz.h"

/* Ideally this would be in a .h file, but it hardly seems worth the trouble */
extern const char *select_default_timezone(const char *share_path);


#ifndef SYSTEMTZDIR
static char tzdirpath[MAXPGPATH];
#endif


/*
 * Return full pathname of timezone data directory
 *
 * In this file, tzdirpath is assumed to be set up by select_default_timezone.
 */
static const char *
pg_TZDIR(void)
{
#ifndef SYSTEMTZDIR
	/* normal case: timezone stuff is under our share dir */
	return tzdirpath;
#else
	/* we're configured to use system's timezone database */
	return SYSTEMTZDIR;
#endif
}


/*
 * Given a timezone name, open() the timezone data file.  Return the
 * file descriptor if successful, -1 if not.
 *
 * This is simpler than the backend function of the same name because
 * we assume that the input string has the correct case already, so there
 * is no need for case-folding.  (This is obviously true if we got the file
 * name from the filesystem to start with.  The only other place it can come
 * from is the environment variable TZ, and there seems no need to allow
 * case variation in that; other programs aren't likely to.)
 *
 * If "canonname" is not NULL, then on success the canonical spelling of the
 * given name is stored there (the buffer must be > TZ_STRLEN_MAX bytes!).
 * This is redundant but kept for compatibility with the backend code.
 */
int
pg_open_tzfile(const char *name, char *canonname)
{
	char		fullname[MAXPGPATH];

	if (canonname)
		strlcpy(canonname, name, TZ_STRLEN_MAX + 1);

	strlcpy(fullname, pg_TZDIR(), sizeof(fullname));
	if (strlen(fullname) + 1 + strlen(name) >= MAXPGPATH)
		return -1;				/* not gonna fit */
	strcat(fullname, "/");
	strcat(fullname, name);

	return open(fullname, O_RDONLY | PG_BINARY, 0);
}



/*
 * Load a timezone definition.
 * Does not verify that the timezone is acceptable!
 *
 * This corresponds to the backend's pg_tzset(), except that we only support
 * one loaded timezone at a time.
 */
static pg_tz *
pg_load_tz(const char *name)
{
	static pg_tz tz;

	if (strlen(name) > TZ_STRLEN_MAX)
		return NULL;			/* not going to fit */

	/*
	 * "GMT" is always sent to tzparse(); see comments for pg_tzset().
	 */
	if (strcmp(name, "GMT") == 0)
	{
		if (tzparse(name, &tz.state, TRUE) != 0)
		{
			/* This really, really should not happen ... */
			return NULL;
		}
	}
	else if (tzload(name, NULL, &tz.state, TRUE) != 0)
	{
		if (name[0] == ':' || tzparse(name, &tz.state, FALSE) != 0)
		{
			return NULL;		/* unknown timezone */
		}
	}

	strcpy(tz.TZname, name);

	return &tz;
}


/*
 * The following block of code attempts to determine which timezone in our
 * timezone database is the best match for the active system timezone.
 *
 * On most systems, we rely on trying to match the observable behavior of
 * the C library's localtime() function.  The database zone that matches
 * furthest into the past is the one to use.  Often there will be several
 * zones with identical rankings (since the Olson database assigns multiple
 * names to many zones).  We break ties arbitrarily by preferring shorter,
 * then alphabetically earlier zone names.
 *
 * Win32's native knowledge about timezones appears to be too incomplete
 * and too different from the Olson database for the above matching strategy
 * to be of any use. But there is just a limited number of timezones
 * available, so we can rely on a handmade mapping table instead.
 */

#ifndef WIN32

#define T_DAY	((time_t) (60*60*24))
#define T_WEEK	((time_t) (60*60*24*7))
#define T_MONTH ((time_t) (60*60*24*31))

#define MAX_TEST_TIMES (52*100) /* 100 years */

struct tztry
{
	int			n_test_times;
	time_t		test_times[MAX_TEST_TIMES];
};

static void scan_available_timezones(char *tzdir, char *tzdirsub,
						 struct tztry * tt,
						 int *bestscore, char *bestzonename);


/*
 * Get GMT offset from a system struct tm
 */
static int
get_timezone_offset(struct tm * tm)
{
#if defined(HAVE_STRUCT_TM_TM_ZONE)
	return tm->tm_gmtoff;
#elif defined(HAVE_INT_TIMEZONE)
	return -TIMEZONE_GLOBAL;
#else
#error No way to determine TZ? Can this happen?
#endif
}

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
score_timezone(const char *tzname, struct tztry * tt)
{
	int			i;
	pg_time_t	pgtt;
	struct tm  *systm;
	struct pg_tm *pgtm;
	char		cbuf[TZ_STRLEN_MAX + 1];
	pg_tz	   *tz;

	/* Load timezone definition */
	tz = pg_load_tz(tzname);
	if (!tz)
		return -1;				/* unrecognized zone name */

	/* Reject if leap seconds involved */
	if (!pg_tz_acceptable(tz))
	{
#ifdef DEBUG_IDENTIFY_TIMEZONE
		fprintf(stderr, "Reject TZ \"%s\": uses leap seconds\n", tzname);
#endif
		return -1;
	}

	/* Check for match at all the test times */
	for (i = 0; i < tt->n_test_times; i++)
	{
		pgtt = (pg_time_t) (tt->test_times[i]);
		pgtm = pg_localtime(&pgtt, tz);
		if (!pgtm)
			return -1;			/* probably shouldn't happen */
		systm = localtime(&(tt->test_times[i]));
		if (!systm)
		{
#ifdef DEBUG_IDENTIFY_TIMEZONE
			fprintf(stderr, "TZ \"%s\" scores %d: at %ld %04d-%02d-%02d %02d:%02d:%02d %s, system had no data\n",
					tzname, i, (long) pgtt,
					pgtm->tm_year + 1900, pgtm->tm_mon + 1, pgtm->tm_mday,
					pgtm->tm_hour, pgtm->tm_min, pgtm->tm_sec,
					pgtm->tm_isdst ? "dst" : "std");
#endif
			return i;
		}
		if (!compare_tm(systm, pgtm))
		{
#ifdef DEBUG_IDENTIFY_TIMEZONE
			fprintf(stderr, "TZ \"%s\" scores %d: at %ld %04d-%02d-%02d %02d:%02d:%02d %s versus %04d-%02d-%02d %02d:%02d:%02d %s\n",
					tzname, i, (long) pgtt,
					pgtm->tm_year + 1900, pgtm->tm_mon + 1, pgtm->tm_mday,
					pgtm->tm_hour, pgtm->tm_min, pgtm->tm_sec,
					pgtm->tm_isdst ? "dst" : "std",
					systm->tm_year + 1900, systm->tm_mon + 1, systm->tm_mday,
					systm->tm_hour, systm->tm_min, systm->tm_sec,
					systm->tm_isdst ? "dst" : "std");
#endif
			return i;
		}
		if (systm->tm_isdst >= 0)
		{
			/* Check match of zone names, too */
			if (pgtm->tm_zone == NULL)
				return -1;		/* probably shouldn't happen */
			memset(cbuf, 0, sizeof(cbuf));
			strftime(cbuf, sizeof(cbuf) - 1, "%Z", systm);		/* zone abbr */
			if (strcmp(cbuf, pgtm->tm_zone) != 0)
			{
#ifdef DEBUG_IDENTIFY_TIMEZONE
				fprintf(stderr, "TZ \"%s\" scores %d: at %ld \"%s\" versus \"%s\"\n",
						tzname, i, (long) pgtt,
						pgtm->tm_zone, cbuf);
#endif
				return i;
			}
		}
	}

#ifdef DEBUG_IDENTIFY_TIMEZONE
	fprintf(stderr, "TZ \"%s\" gets max score %d\n", tzname, i);
#endif

	return i;
}


/*
 * Try to identify a timezone name (in our terminology) that best matches the
 * observed behavior of the system timezone library.  We cannot assume that
 * the system TZ environment setting (if indeed there is one) matches our
 * terminology, so we ignore it and just look at what localtime() returns.
 */
static const char *
identify_system_timezone(void)
{
	static char resultbuf[TZ_STRLEN_MAX + 1];
	time_t		tnow;
	time_t		t;
	struct tztry tt;
	struct tm  *tm;
	int			thisyear;
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
	 * matches the system zone.  We first probe January and July of the
	 * current year; this serves to quickly eliminate the vast majority of the
	 * TZ database entries.  If those dates match, we probe every week for 100
	 * years backwards from the current July.  (Weekly resolution is good
	 * enough to identify DST transition rules, since everybody switches on
	 * Sundays.)  This is sufficient to cover most of the Unix time_t range,
	 * and we don't want to look further than that since many systems won't
	 * have sane TZ behavior further back anyway.  The further back the zone
	 * matches, the better we score it.  This may seem like a rather random
	 * way of doing things, but experience has shown that system-supplied
	 * timezone definitions are likely to have DST behavior that is right for
	 * the recent past and not so accurate further back. Scoring in this way
	 * allows us to recognize zones that have some commonality with the Olson
	 * database, without insisting on exact match. (Note: we probe Thursdays,
	 * not Sundays, to avoid triggering DST-transition bugs in localtime
	 * itself.)
	 */
	tnow = time(NULL);
	tm = localtime(&tnow);
	if (!tm)
		return NULL;			/* give up if localtime is broken... */
	thisyear = tm->tm_year + 1900;

	t = build_time_t(thisyear, 1, 15);

	/*
	 * Round back to GMT midnight Thursday.  This depends on the knowledge
	 * that the time_t origin is Thu Jan 01 1970.  (With a different origin
	 * we'd be probing some other day of the week, but it wouldn't matter
	 * anyway unless localtime() had DST-transition bugs.)
	 */
	t -= (t % T_WEEK);

	tt.n_test_times = 0;
	tt.test_times[tt.n_test_times++] = t;

	t = build_time_t(thisyear, 7, 15);
	t -= (t % T_WEEK);

	tt.test_times[tt.n_test_times++] = t;

	while (tt.n_test_times < MAX_TEST_TIMES)
	{
		t -= T_WEEK;
		tt.test_times[tt.n_test_times++] = t;
	}

	/* Search for the best-matching timezone file */
	strlcpy(tmptzdir, pg_TZDIR(), sizeof(tmptzdir));
	bestscore = -1;
	resultbuf[0] = '\0';
	scan_available_timezones(tmptzdir, tmptzdir + strlen(tmptzdir) + 1,
							 &tt,
							 &bestscore, resultbuf);
	if (bestscore > 0)
	{
		/* Ignore Olson's rather silly "Factory" zone; use GMT instead */
		if (strcmp(resultbuf, "Factory") == 0)
			return NULL;
		return resultbuf;
	}

	/*
	 * Couldn't find a match in the database, so next we try constructed zone
	 * names (like "PST8PDT").
	 *
	 * First we need to determine the names of the local standard and daylight
	 * zones.  The idea here is to scan forward from today until we have seen
	 * both zones, if both are in use.
	 */
	memset(std_zone_name, 0, sizeof(std_zone_name));
	memset(dst_zone_name, 0, sizeof(dst_zone_name));
	std_ofs = 0;

	tnow = time(NULL);

	/*
	 * Round back to a GMT midnight so results don't depend on local time of
	 * day
	 */
	tnow -= (tnow % T_DAY);

	/*
	 * We have to look a little further ahead than one year, in case today is
	 * just past a DST boundary that falls earlier in the year than the next
	 * similar boundary.  Arbitrarily scan up to 14 months.
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
			strcpy(std_zone_name, cbuf);
			std_ofs = get_timezone_offset(tm);
		}
		if (tm->tm_isdst > 0 && dst_zone_name[0] == '\0')
		{
			/* found DST zone */
			memset(cbuf, 0, sizeof(cbuf));
			strftime(cbuf, sizeof(cbuf) - 1, "%Z", tm); /* zone abbr */
			strcpy(dst_zone_name, cbuf);
		}
		/* Done if found both */
		if (std_zone_name[0] && dst_zone_name[0])
			break;
	}

	/* We should have found a STD zone name by now... */
	if (std_zone_name[0] == '\0')
	{
#ifdef DEBUG_IDENTIFY_TIMEZONE
		fprintf(stderr, "could not determine system time zone\n");
#endif
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
	 * Olson timezone database names the GMT-offset zones in POSIX style: plus
	 * is west of Greenwich.  It's unfortunate that this is opposite of SQL
	 * conventions.  Should we therefore change the names? Probably not...
	 */
	snprintf(resultbuf, sizeof(resultbuf), "Etc/GMT%s%d",
			 (-std_ofs > 0) ? "+" : "", -std_ofs / 3600);

#ifdef DEBUG_IDENTIFY_TIMEZONE
	fprintf(stderr, "could not recognize system time zone, using \"%s\"\n",
			resultbuf);
#endif
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
scan_available_timezones(char *tzdir, char *tzdirsub, struct tztry * tt,
						 int *bestscore, char *bestzonename)
{
	int			tzdir_orig_len = strlen(tzdir);
	char	  **names;
	char	  **namep;

	names = pgfnames(tzdir);
	if (!names)
		return;

	for (namep = names; *namep; namep++)
	{
		char	   *name = *namep;
		struct stat statbuf;

		/* Ignore . and .., plus any other "hidden" files */
		if (name[0] == '.')
			continue;

		snprintf(tzdir + tzdir_orig_len, MAXPGPATH - tzdir_orig_len,
				 "/%s", name);

		if (stat(tzdir, &statbuf) != 0)
		{
#ifdef DEBUG_IDENTIFY_TIMEZONE
			fprintf(stderr, "could not stat \"%s\": %s\n",
					tzdir, strerror(errno));
#endif
			tzdir[tzdir_orig_len] = '\0';
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
			int			score = score_timezone(tzdirsub, tt);

			if (score > *bestscore)
			{
				*bestscore = score;
				strlcpy(bestzonename, tzdirsub, TZ_STRLEN_MAX + 1);
			}
			else if (score == *bestscore)
			{
				/* Consider how to break a tie */
				if (strlen(tzdirsub) < strlen(bestzonename) ||
					(strlen(tzdirsub) == strlen(bestzonename) &&
					 strcmp(tzdirsub, bestzonename) < 0))
					strlcpy(bestzonename, tzdirsub, TZ_STRLEN_MAX + 1);
			}
		}

		/* Restore tzdir */
		tzdir[tzdir_orig_len] = '\0';
	}

	pgfnames_cleanup(names);
}
#else							/* WIN32 */

static const struct
{
	const char *stdname;		/* Windows name of standard timezone */
	const char *dstname;		/* Windows name of daylight timezone */
	const char *pgtzname;		/* Name of pgsql timezone to map to */
}	win32_tzmap[] =

{
	/*
	 * This list was built from the contents of the registry at
	 * HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Time
	 * Zones on Windows 2003 R2.
	 *
	 * The zones have been matched to Olson timezones by looking at the cities
	 * listed in the win32 display name (in the comment here) in most cases.
	 */
	{
		"Afghanistan Standard Time", "Afghanistan Daylight Time",
		"Asia/Kabul"
	},							/* (GMT+04:30) Kabul */
	{
		"Alaskan Standard Time", "Alaskan Daylight Time",
		"US/Alaska"
	},							/* (GMT-09:00) Alaska */
	{
		"Arab Standard Time", "Arab Daylight Time",
		"Asia/Kuwait"
	},							/* (GMT+03:00) Kuwait, Riyadh */
	{
		"Arabian Standard Time", "Arabian Daylight Time",
		"Asia/Muscat"
	},							/* (GMT+04:00) Abu Dhabi, Muscat */
	{
		"Arabic Standard Time", "Arabic Daylight Time",
		"Asia/Baghdad"
	},							/* (GMT+03:00) Baghdad */
	{
		"Argentina Standard Time", "Argentina Daylight Time",
		"America/Buenos_Aires"
	},							/* (GMT-03:00) Buenos Aires */
	{
		"Armenian Standard Time", "Armenian Daylight Time",
		"Asia/Yerevan"
	},							/* (GMT+04:00) Yerevan */
	{
		"Atlantic Standard Time", "Atlantic Daylight Time",
		"Canada/Atlantic"
	},							/* (GMT-04:00) Atlantic Time (Canada) */
	{
		"AUS Central Standard Time", "AUS Central Daylight Time",
		"Australia/Darwin"
	},							/* (GMT+09:30) Darwin */
	{
		"AUS Eastern Standard Time", "AUS Eastern Daylight Time",
		"Australia/Canberra"
	},							/* (GMT+10:00) Canberra, Melbourne, Sydney */
	{
		"Azerbaijan Standard Time", "Azerbaijan Daylight Time",
		"Asia/Baku"
	},							/* (GMT+04:00) Baku */
	{
		"Azores Standard Time", "Azores Daylight Time",
		"Atlantic/Azores"
	},							/* (GMT-01:00) Azores */
	{
		"Bangladesh Standard Time", "Bangladesh Daylight Time",
		"Asia/Dhaka"
	},							/* (GMT+06:00) Dhaka */
	{
		"Canada Central Standard Time", "Canada Central Daylight Time",
		"Canada/Saskatchewan"
	},							/* (GMT-06:00) Saskatchewan */
	{
		"Cape Verde Standard Time", "Cape Verde Daylight Time",
		"Atlantic/Cape_Verde"
	},							/* (GMT-01:00) Cape Verde Is. */
	{
		"Caucasus Standard Time", "Caucasus Daylight Time",
		"Asia/Baku"
	},							/* (GMT+04:00) Baku, Tbilisi, Yerevan */
	{
		"Cen. Australia Standard Time", "Cen. Australia Daylight Time",
		"Australia/Adelaide"
	},							/* (GMT+09:30) Adelaide */
	/* Central America (other than Mexico) generally does not observe DST */
	{
		"Central America Standard Time", "Central America Daylight Time",
		"CST6"
	},							/* (GMT-06:00) Central America */
	{
		"Central Asia Standard Time", "Central Asia Daylight Time",
		"Asia/Dhaka"
	},							/* (GMT+06:00) Astana, Dhaka */
	{
		"Central Brazilian Standard Time", "Central Brazilian Daylight Time",
		"America/Cuiaba"
	},							/* (GMT-04:00) Cuiaba */
	{
		"Central Europe Standard Time", "Central Europe Daylight Time",
		"Europe/Belgrade"
	},							/* (GMT+01:00) Belgrade, Bratislava, Budapest,
								 * Ljubljana, Prague */
	{
		"Central European Standard Time", "Central European Daylight Time",
		"Europe/Sarajevo"
	},							/* (GMT+01:00) Sarajevo, Skopje, Warsaw,
								 * Zagreb */
	{
		"Central Pacific Standard Time", "Central Pacific Daylight Time",
		"Pacific/Noumea"
	},							/* (GMT+11:00) Magadan, Solomon Is., New
								 * Caledonia */
	{
		"Central Standard Time", "Central Daylight Time",
		"US/Central"
	},							/* (GMT-06:00) Central Time (US & Canada) */
	{
		"Central Standard Time (Mexico)", "Central Daylight Time (Mexico)",
		"America/Mexico_City"
	},							/* (GMT-06:00) Guadalajara, Mexico City,
								 * Monterrey - New */
	{
		"China Standard Time", "China Daylight Time",
		"Asia/Hong_Kong"
	},							/* (GMT+08:00) Beijing, Chongqing, Hong Kong,
								 * Urumqi */
	{
		"Dateline Standard Time", "Dateline Daylight Time",
		"Etc/GMT+12"
	},							/* (GMT-12:00) International Date Line West */
	{
		"E. Africa Standard Time", "E. Africa Daylight Time",
		"Africa/Nairobi"
	},							/* (GMT+03:00) Nairobi */
	{
		"E. Australia Standard Time", "E. Australia Daylight Time",
		"Australia/Brisbane"
	},							/* (GMT+10:00) Brisbane */
	{
		"E. Europe Standard Time", "E. Europe Daylight Time",
		"Europe/Bucharest"
	},							/* (GMT+02:00) Bucharest */
	{
		"E. South America Standard Time", "E. South America Daylight Time",
		"America/Araguaina"
	},							/* (GMT-03:00) Brasilia */
	{
		"Eastern Standard Time", "Eastern Daylight Time",
		"US/Eastern"
	},							/* (GMT-05:00) Eastern Time (US & Canada) */
	{
		"Egypt Standard Time", "Egypt Daylight Time",
		"Africa/Cairo"
	},							/* (GMT+02:00) Cairo */
	{
		"Ekaterinburg Standard Time", "Ekaterinburg Daylight Time",
		"Asia/Yekaterinburg"
	},							/* (GMT+05:00) Ekaterinburg */
	{
		"Fiji Standard Time", "Fiji Daylight Time",
		"Pacific/Fiji"
	},							/* (GMT+12:00) Fiji, Kamchatka, Marshall Is. */
	{
		"FLE Standard Time", "FLE Daylight Time",
		"Europe/Helsinki"
	},							/* (GMT+02:00) Helsinki, Kyiv, Riga, Sofia,
								 * Tallinn, Vilnius */
	{
		"Georgian Standard Time", "Georgian Daylight Time",
		"Asia/Tbilisi"
	},							/* (GMT+03:00) Tbilisi */
	{
		"GMT Standard Time", "GMT Daylight Time",
		"Europe/London"
	},							/* (GMT) Greenwich Mean Time : Dublin,
								 * Edinburgh, Lisbon, London */
	{
		"Greenland Standard Time", "Greenland Daylight Time",
		"America/Godthab"
	},							/* (GMT-03:00) Greenland */
	{
		"Greenwich Standard Time", "Greenwich Daylight Time",
		"Africa/Casablanca"
	},							/* (GMT) Casablanca, Monrovia */
	{
		"GTB Standard Time", "GTB Daylight Time",
		"Europe/Athens"
	},							/* (GMT+02:00) Athens, Istanbul, Minsk */
	{
		"Hawaiian Standard Time", "Hawaiian Daylight Time",
		"US/Hawaii"
	},							/* (GMT-10:00) Hawaii */
	{
		"India Standard Time", "India Daylight Time",
		"Asia/Calcutta"
	},							/* (GMT+05:30) Chennai, Kolkata, Mumbai, New
								 * Delhi */
	{
		"Iran Standard Time", "Iran Daylight Time",
		"Asia/Tehran"
	},							/* (GMT+03:30) Tehran */
	{
		"Jerusalem Standard Time", "Jerusalem Daylight Time",
		"Asia/Jerusalem"
	},							/* (GMT+02:00) Jerusalem */
	{
		"Jordan Standard Time", "Jordan Daylight Time",
		"Asia/Amman"
	},							/* (GMT+02:00) Amman */
	{
		"Kamchatka Standard Time", "Kamchatka Daylight Time",
		"Asia/Kamchatka"
	},							/* (GMT+12:00) Petropavlovsk-Kamchatsky */
	{
		"Korea Standard Time", "Korea Daylight Time",
		"Asia/Seoul"
	},							/* (GMT+09:00) Seoul */
	{
		"Mauritius Standard Time", "Mauritius Daylight Time",
		"Indian/Mauritius"
	},							/* (GMT+04:00) Port Louis */
	{
		"Mexico Standard Time", "Mexico Daylight Time",
		"America/Mexico_City"
	},							/* (GMT-06:00) Guadalajara, Mexico City,
								 * Monterrey */
	{
		"Mexico Standard Time 2", "Mexico Daylight Time 2",
		"America/Chihuahua"
	},							/* (GMT-07:00) Chihuahua, La Paz, Mazatlan */
	{
		"Mid-Atlantic Standard Time", "Mid-Atlantic Daylight Time",
		"Atlantic/South_Georgia"
	},							/* (GMT-02:00) Mid-Atlantic */
	{
		"Middle East Standard Time", "Middle East Daylight Time",
		"Asia/Beirut"
	},							/* (GMT+02:00) Beirut */
	{
		"Montevideo Standard Time", "Montevideo Daylight Time",
		"America/Montevideo"
	},							/* (GMT-03:00) Montevideo */
	{
		"Morocco Standard Time", "Morocco Daylight Time",
		"Africa/Casablanca"
	},							/* (GMT) Casablanca */
	{
		"Mountain Standard Time", "Mountain Daylight Time",
		"US/Mountain"
	},							/* (GMT-07:00) Mountain Time (US & Canada) */
	{
		"Mountain Standard Time (Mexico)", "Mountain Daylight Time (Mexico)",
		"America/Chihuahua"
	},							/* (GMT-07:00) Chihuahua, La Paz, Mazatlan -
								 * New */
	{
		"Myanmar Standard Time", "Myanmar Daylight Time",
		"Asia/Rangoon"
	},							/* (GMT+06:30) Rangoon */
	{
		"N. Central Asia Standard Time", "N. Central Asia Daylight Time",
		"Asia/Novosibirsk"
	},							/* (GMT+06:00) Novosibirsk */
	{
		"Namibia Standard Time", "Namibia Daylight Time",
		"Africa/Windhoek"
	},							/* (GMT+02:00) Windhoek */
	{
		"Nepal Standard Time", "Nepal Daylight Time",
		"Asia/Katmandu"
	},							/* (GMT+05:45) Kathmandu */
	{
		"New Zealand Standard Time", "New Zealand Daylight Time",
		"Pacific/Auckland"
	},							/* (GMT+12:00) Auckland, Wellington */
	{
		"Newfoundland Standard Time", "Newfoundland Daylight Time",
		"Canada/Newfoundland"
	},							/* (GMT-03:30) Newfoundland */
	{
		"North Asia East Standard Time", "North Asia East Daylight Time",
		"Asia/Irkutsk"
	},							/* (GMT+08:00) Irkutsk, Ulaan Bataar */
	{
		"North Asia Standard Time", "North Asia Daylight Time",
		"Asia/Krasnoyarsk"
	},							/* (GMT+07:00) Krasnoyarsk */
	{
		"Pacific SA Standard Time", "Pacific SA Daylight Time",
		"America/Santiago"
	},							/* (GMT-04:00) Santiago */
	{
		"Pacific Standard Time", "Pacific Daylight Time",
		"US/Pacific"
	},							/* (GMT-08:00) Pacific Time (US & Canada);
								 * Tijuana */
	{
		"Pacific Standard Time (Mexico)", "Pacific Daylight Time (Mexico)",
		"America/Tijuana"
	},							/* (GMT-08:00) Tijuana, Baja California */
	{
		"Pakistan Standard Time", "Pakistan Daylight Time",
		"Asia/Karachi"
	},							/* (GMT+05:00) Islamabad, Karachi */
	{
		"Paraguay Standard Time", "Paraguay Daylight Time",
		"America/Asuncion"
	},							/* (GMT-04:00) Asuncion */
	{
		"Romance Standard Time", "Romance Daylight Time",
		"Europe/Brussels"
	},							/* (GMT+01:00) Brussels, Copenhagen, Madrid,
								 * Paris */
	{
		"Russian Standard Time", "Russian Daylight Time",
		"Europe/Moscow"
	},							/* (GMT+03:00) Moscow, St. Petersburg,
								 * Volgograd */
	{
		"SA Eastern Standard Time", "SA Eastern Daylight Time",
		"America/Buenos_Aires"
	},							/* (GMT-03:00) Buenos Aires, Georgetown */
	{
		"SA Pacific Standard Time", "SA Pacific Daylight Time",
		"America/Bogota"
	},							/* (GMT-05:00) Bogota, Lima, Quito */
	{
		"SA Western Standard Time", "SA Western Daylight Time",
		"America/Caracas"
	},							/* (GMT-04:00) Caracas, La Paz */
	{
		"Samoa Standard Time", "Samoa Daylight Time",
		"Pacific/Midway"
	},							/* (GMT-11:00) Midway Island, Samoa */
	{
		"SE Asia Standard Time", "SE Asia Daylight Time",
		"Asia/Bangkok"
	},							/* (GMT+07:00) Bangkok, Hanoi, Jakarta */
	{
		"Malay Peninsula Standard Time", "Malay Peninsula Daylight Time",
		"Asia/Kuala_Lumpur"
	},							/* (GMT+08:00) Kuala Lumpur, Singapore */
	{
		"South Africa Standard Time", "South Africa Daylight Time",
		"Africa/Harare"
	},							/* (GMT+02:00) Harare, Pretoria */
	{
		"Sri Lanka Standard Time", "Sri Lanka Daylight Time",
		"Asia/Colombo"
	},							/* (GMT+06:00) Sri Jayawardenepura */
	{
		"Taipei Standard Time", "Taipei Daylight Time",
		"Asia/Taipei"
	},							/* (GMT+08:00) Taipei */
	{
		"Tasmania Standard Time", "Tasmania Daylight Time",
		"Australia/Hobart"
	},							/* (GMT+10:00) Hobart */
	{
		"Tokyo Standard Time", "Tokyo Daylight Time",
		"Asia/Tokyo"
	},							/* (GMT+09:00) Osaka, Sapporo, Tokyo */
	{
		"Tonga Standard Time", "Tonga Daylight Time",
		"Pacific/Tongatapu"
	},							/* (GMT+13:00) Nuku'alofa */
	{
		"Ulaanbaatar Standard Time", "Ulaanbaatar Daylight Time",
		"Asia/Ulaanbaatar",
	},							/* (GMT+08:00) Ulaanbaatar */
	{
		"US Eastern Standard Time", "US Eastern Daylight Time",
		"US/Eastern"
	},							/* (GMT-05:00) Indiana (East) */
	{
		"US Mountain Standard Time", "US Mountain Daylight Time",
		"US/Arizona"
	},							/* (GMT-07:00) Arizona */
	{
		"Coordinated Universal Time", "Coordinated Universal Time",
		"UTC"
	},							/* (GMT) Coordinated Universal Time */
	{
		"UTC+12", "UTC+12",
		"Etc/GMT+12"
	},							/* (GMT+12:00) Coordinated Universal Time+12 */
	{
		"UTC-02", "UTC-02",
		"Etc/GMT-02"
	},							/* (GMT-02:00) Coordinated Universal Time-02 */
	{
		"UTC-11", "UTC-11",
		"Etc/GMT-11"
	},							/* (GMT-11:00) Coordinated Universal Time-11 */
	{
		"Venezuela Standard Time", "Venezuela Daylight Time",
		"America/Caracas",
	},							/* (GMT-04:30) Caracas */
	{
		"Vladivostok Standard Time", "Vladivostok Daylight Time",
		"Asia/Vladivostok"
	},							/* (GMT+10:00) Vladivostok */
	{
		"W. Australia Standard Time", "W. Australia Daylight Time",
		"Australia/Perth"
	},							/* (GMT+08:00) Perth */
#ifdef NOT_USED
	/* Could not find a match for this one (just a guess). Excluded for now. */
	{
		"W. Central Africa Standard Time", "W. Central Africa Daylight Time",
		"WAT"
	},							/* (GMT+01:00) West Central Africa */
#endif
	{
		"W. Europe Standard Time", "W. Europe Daylight Time",
		"CET"
	},							/* (GMT+01:00) Amsterdam, Berlin, Bern, Rome,
								 * Stockholm, Vienna */
	{
		"West Asia Standard Time", "West Asia Daylight Time",
		"Asia/Karachi"
	},							/* (GMT+05:00) Islamabad, Karachi, Tashkent */
	{
		"West Pacific Standard Time", "West Pacific Daylight Time",
		"Pacific/Guam"
	},							/* (GMT+10:00) Guam, Port Moresby */
	{
		"Yakutsk Standard Time", "Yakutsk Daylight Time",
		"Asia/Yakutsk"
	},							/* (GMT+09:00) Yakutsk */
	{
		NULL, NULL, NULL
	}
};

static const char *
identify_system_timezone(void)
{
	int			i;
	char		tzname[128];
	char		localtzname[256];
	time_t		t = time(NULL);
	struct tm  *tm = localtime(&t);
	HKEY		rootKey;
	int			idx;

	if (!tm)
	{
#ifdef DEBUG_IDENTIFY_TIMEZONE
		fprintf(stderr, "could not identify system time zone: localtime() failed\n");
#endif
		return NULL;			/* go to GMT */
	}

	memset(tzname, 0, sizeof(tzname));
	strftime(tzname, sizeof(tzname) - 1, "%Z", tm);

	for (i = 0; win32_tzmap[i].stdname != NULL; i++)
	{
		if (strcmp(tzname, win32_tzmap[i].stdname) == 0 ||
			strcmp(tzname, win32_tzmap[i].dstname) == 0)
		{
#ifdef DEBUG_IDENTIFY_TIMEZONE
			fprintf(stderr, "TZ \"%s\" matches system time zone \"%s\"\n",
					win32_tzmap[i].pgtzname, tzname);
#endif
			return win32_tzmap[i].pgtzname;
		}
	}

	/*
	 * Localized Windows versions return localized names for the timezone.
	 * Scan the registry to find the English name, and then try matching
	 * against our table again.
	 */
	memset(localtzname, 0, sizeof(localtzname));
	if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
			   "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones",
					 0,
					 KEY_READ,
					 &rootKey) != ERROR_SUCCESS)
	{
#ifdef DEBUG_IDENTIFY_TIMEZONE
		fprintf(stderr, "could not open registry key to identify system time zone: error code %lu\n",
				GetLastError());
#endif
		return NULL;			/* go to GMT */
	}

	for (idx = 0;; idx++)
	{
		char		keyname[256];
		char		zonename[256];
		DWORD		namesize;
		FILETIME	lastwrite;
		HKEY		key;
		LONG		r;

		memset(keyname, 0, sizeof(keyname));
		namesize = sizeof(keyname);
		if ((r = RegEnumKeyEx(rootKey,
							  idx,
							  keyname,
							  &namesize,
							  NULL,
							  NULL,
							  NULL,
							  &lastwrite)) != ERROR_SUCCESS)
		{
			if (r == ERROR_NO_MORE_ITEMS)
				break;
#ifdef DEBUG_IDENTIFY_TIMEZONE
			fprintf(stderr, "could not enumerate registry subkeys to identify system time zone: %d\n",
					(int) r);
#endif
			break;
		}

		if ((r = RegOpenKeyEx(rootKey, keyname, 0, KEY_READ, &key)) != ERROR_SUCCESS)
		{
#ifdef DEBUG_IDENTIFY_TIMEZONE
			fprintf(stderr, "could not open registry subkey to identify system time zone: %d\n",
					(int) r);
#endif
			break;
		}

		memset(zonename, 0, sizeof(zonename));
		namesize = sizeof(zonename);
		if ((r = RegQueryValueEx(key, "Std", NULL, NULL, (unsigned char *) zonename, &namesize)) != ERROR_SUCCESS)
		{
#ifdef DEBUG_IDENTIFY_TIMEZONE
			fprintf(stderr, "could not query value for key \"std\" to identify system time zone \"%s\": %d\n",
					keyname, (int) r);
#endif
			RegCloseKey(key);
			continue;			/* Proceed to look at the next timezone */
		}
		if (strcmp(tzname, zonename) == 0)
		{
			/* Matched zone */
			strcpy(localtzname, keyname);
			RegCloseKey(key);
			break;
		}
		memset(zonename, 0, sizeof(zonename));
		namesize = sizeof(zonename);
		if ((r = RegQueryValueEx(key, "Dlt", NULL, NULL, (unsigned char *) zonename, &namesize)) != ERROR_SUCCESS)
		{
#ifdef DEBUG_IDENTIFY_TIMEZONE
			fprintf(stderr, "could not query value for key \"dlt\" to identify system time zone \"%s\": %d\n",
					keyname, (int) r);
#endif
			RegCloseKey(key);
			continue;			/* Proceed to look at the next timezone */
		}
		if (strcmp(tzname, zonename) == 0)
		{
			/* Matched DST zone */
			strcpy(localtzname, keyname);
			RegCloseKey(key);
			break;
		}

		RegCloseKey(key);
	}

	RegCloseKey(rootKey);

	if (localtzname[0])
	{
		/* Found a localized name, so scan for that one too */
		for (i = 0; win32_tzmap[i].stdname != NULL; i++)
		{
			if (strcmp(localtzname, win32_tzmap[i].stdname) == 0 ||
				strcmp(localtzname, win32_tzmap[i].dstname) == 0)
			{
#ifdef DEBUG_IDENTIFY_TIMEZONE
				fprintf(stderr, "TZ \"%s\" matches localized system time zone \"%s\" (\"%s\")\n",
						win32_tzmap[i].pgtzname, tzname, localtzname);
#endif
				return win32_tzmap[i].pgtzname;
			}
		}
	}

#ifdef DEBUG_IDENTIFY_TIMEZONE
	fprintf(stderr, "could not find a match for system time zone \"%s\"\n",
			tzname);
#endif
	return NULL;				/* go to GMT */
}
#endif   /* WIN32 */


/*
 * Return true if the given zone name is valid and is an "acceptable" zone.
 */
static bool
validate_zone(const char *tzname)
{
	pg_tz	   *tz;

	if (!tzname || !tzname[0])
		return false;

	tz = pg_load_tz(tzname);
	if (!tz)
		return false;

	if (!pg_tz_acceptable(tz))
		return false;

	return true;
}

/*
 * Identify a suitable default timezone setting based on the environment.
 *
 * The installation share_path must be passed in, as that is the default
 * location for the timezone database directory.
 *
 * We first look to the TZ environment variable.  If not found or not
 * recognized by our own code, we see if we can identify the timezone
 * from the behavior of the system timezone library.  When all else fails,
 * return NULL, indicating that we should default to GMT.
 */
const char *
select_default_timezone(const char *share_path)
{
	const char *tzname;

	/* Initialize timezone directory path, if needed */
#ifndef SYSTEMTZDIR
	snprintf(tzdirpath, sizeof(tzdirpath), "%s/timezone", share_path);
#endif

	/* Check TZ environment variable */
	tzname = getenv("TZ");
	if (validate_zone(tzname))
		return tzname;

	/* Nope, so try to identify the system timezone */
	tzname = identify_system_timezone();
	if (validate_zone(tzname))
		return tzname;

	return NULL;
}
