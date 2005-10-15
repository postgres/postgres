#ifndef TZFILE_H
#define TZFILE_H

/*
 * This file is in the public domain, so clarified as of
 * 1996-06-05 by Arthur David Olson (arthur_david_olson@nih.gov).
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/timezone/tzfile.h,v 1.6 2005/10/15 02:49:51 momjian Exp $
 */

/*
 * This header is for use ONLY with the time conversion code.
 * There is no guarantee that it will remain unchanged,
 * or that it will remain at all.
 * Do NOT copy it to any system include directory.
 * Thank you!
 */

/*
 * Information about time zone files.
 */

#define TZDEFAULT	"localtime"
#define TZDEFRULES	"posixrules"

/*
 * Each file begins with. . .
 */

#define TZ_MAGIC	"TZif"

struct tzhead
{
	char		tzh_magic[4];	/* TZ_MAGIC */
	char		tzh_reserved[16];		/* reserved for future use */
	char		tzh_ttisgmtcnt[4];		/* coded number of trans. time flags */
	char		tzh_ttisstdcnt[4];		/* coded number of trans. time flags */
	char		tzh_leapcnt[4]; /* coded number of leap seconds */
	char		tzh_timecnt[4]; /* coded number of transition times */
	char		tzh_typecnt[4]; /* coded number of local time types */
	char		tzh_charcnt[4]; /* coded number of abbr. chars */
};

/*----------
 * . . .followed by. . .
 *
 *	tzh_timecnt (char [4])s		coded transition times a la time(2)
 *	tzh_timecnt (unsigned char)s	types of local time starting at above
 *	tzh_typecnt repetitions of
 *		one (char [4])		coded UTC offset in seconds
 *		one (unsigned char) used to set tm_isdst
 *		one (unsigned char) that's an abbreviation list index
 *	tzh_charcnt (char)s		'\0'-terminated zone abbreviations
 *	tzh_leapcnt repetitions of
 *		one (char [4])		coded leap second transition times
 *		one (char [4])		total correction after above
 *	tzh_ttisstdcnt (char)s		indexed by type; if TRUE, transition
 *					time is standard time, if FALSE,
 *					transition time is wall clock time
 *					if absent, transition times are
 *					assumed to be wall clock time
 *	tzh_ttisgmtcnt (char)s		indexed by type; if TRUE, transition
 *					time is UTC, if FALSE,
 *					transition time is local time
 *					if absent, transition times are
 *					assumed to be local time
 *----------
 */

/*
 * In the current implementation, "tzset()" refuses to deal with files that
 * exceed any of the limits below.
 */

/*
 * The TZ_MAX_TIMES value below is enough to handle a bit more than a
 * year's worth of solar time (corrected daily to the nearest second) or
 * 138 years of Pacific Presidential Election time
 * (where there are three time zone transitions every fourth year).
 */
#define TZ_MAX_TIMES	370

#define TZ_MAX_TYPES	256		/* Limited by what (unsigned char)'s can hold */

#define TZ_MAX_CHARS	50		/* Maximum number of abbreviation characters */
 /* (limited by what unsigned chars can hold) */

#define TZ_MAX_LEAPS	50		/* Maximum number of leap second corrections */

#define SECSPERMIN	60
#define MINSPERHOUR 60
#define HOURSPERDAY 24
#define DAYSPERWEEK 7
#define DAYSPERNYEAR	365
#define DAYSPERLYEAR	366
#define SECSPERHOUR (SECSPERMIN * MINSPERHOUR)
#define SECSPERDAY	((long) SECSPERHOUR * HOURSPERDAY)
#define MONSPERYEAR 12

#define TM_SUNDAY	0
#define TM_MONDAY	1
#define TM_TUESDAY	2
#define TM_WEDNESDAY	3
#define TM_THURSDAY 4
#define TM_FRIDAY	5
#define TM_SATURDAY 6

#define TM_JANUARY	0
#define TM_FEBRUARY 1
#define TM_MARCH	2
#define TM_APRIL	3
#define TM_MAY		4
#define TM_JUNE		5
#define TM_JULY		6
#define TM_AUGUST	7
#define TM_SEPTEMBER	8
#define TM_OCTOBER	9
#define TM_NOVEMBER 10
#define TM_DECEMBER 11

#define TM_YEAR_BASE	1900

#define EPOCH_YEAR	1970
#define EPOCH_WDAY	TM_THURSDAY

/*
 * Accurate only for the past couple of centuries;
 * that will probably do.
 */

#define isleap(y) (((y) % 4) == 0 && (((y) % 100) != 0 || ((y) % 400) == 0))

#endif   /* !defined TZFILE_H */
