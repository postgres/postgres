/*-------------------------------------------------------------------------
 *
 * datetime.c
 *	  Support functions for date/time types.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/datetime.c,v 1.44 2000/03/16 14:36:51 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <math.h>
#include <sys/types.h>
#include <errno.h>

#include "postgres.h"
#ifdef HAVE_FLOAT_H
#include <float.h>
#endif
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#ifndef USE_POSIX_TIME
#include <sys/timeb.h>
#endif

#include "miscadmin.h"
#include "utils/datetime.h"


#define USE_DATE_CACHE 1
#define ROUND_ALL 0

static int DecodePosixTimezone(char *str, int *val);

int			day_tab[2][13] = {
	{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 0},
{31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 0}};


char	   *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
"Jul", "Aug", "Sep", "Oct", "Nov", "Dec", NULL};

char	   *days[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
"Thursday", "Friday", "Saturday", NULL};


#define UTIME_MINYEAR (1901)
#define UTIME_MINMONTH (12)
#define UTIME_MINDAY (14)
#define UTIME_MAXYEAR (2038)
#define UTIME_MAXMONTH (01)
#define UTIME_MAXDAY (18)

#define IS_VALID_UTIME(y,m,d) (((y > UTIME_MINYEAR) \
 || ((y == UTIME_MINYEAR) && ((m > UTIME_MINMONTH) \
  || ((m == UTIME_MINMONTH) && (d >= UTIME_MINDAY))))) \
 && ((y < UTIME_MAXYEAR) \
 || ((y == UTIME_MAXYEAR) && ((m < UTIME_MAXMONTH) \
  || ((m == UTIME_MAXMONTH) && (d <= UTIME_MAXDAY))))))


/*****************************************************************************
 *	 PRIVATE ROUTINES														 *
 *****************************************************************************/

/* definitions for squeezing values into "value" */
#define ABS_SIGNBIT		(char) 0200
#define VALMASK			(char) 0177
#define NEG(n)			((n)|ABS_SIGNBIT)
#define SIGNEDCHAR(c)	((c)&ABS_SIGNBIT? -((c)&VALMASK): (c))
#define FROMVAL(tp)		(-SIGNEDCHAR((tp)->value) * 10) /* uncompress */
#define TOVAL(tp, v)	((tp)->value = ((v) < 0? NEG((-(v))/10): (v)/10))

/*
 * to keep this table reasonably small, we divide the lexval for TZ and DTZ
 * entries by 10 and truncate the text field at MAXTOKLEN characters.
 * the text field is not guaranteed to be NULL-terminated.
 */
static datetkn datetktbl[] = {
/*		text			token	lexval */
	{EARLY, RESERV, DTK_EARLY}, /* "-infinity" reserved for "early time" */
	{"acsst", DTZ, 63},			/* Cent. Australia */
	{"acst", TZ, 57},			/* Cent. Australia */
	{DA_D, ADBC, AD},			/* "ad" for years >= 0 */
	{"abstime", IGNORE, 0},		/* "abstime" for pre-v6.1 "Invalid
								 * Abstime" */
	{"adt", DTZ, NEG(18)},		/* Atlantic Daylight Time */
	{"aesst", DTZ, 66},			/* E. Australia */
	{"aest", TZ, 60},			/* Australia Eastern Std Time */
	{"ahst", TZ, NEG(60)},		/* Alaska-Hawaii Std Time */
	{"allballs", RESERV, DTK_ZULU},		/* 00:00:00 */
	{"am", AMPM, AM},
	{"apr", MONTH, 4},
	{"april", MONTH, 4},
	{"ast", TZ, NEG(24)},		/* Atlantic Std Time (Canada) */
	{"at", IGNORE, 0},			/* "at" (throwaway) */
	{"aug", MONTH, 8},
	{"august", MONTH, 8},
	{"awsst", DTZ, 54},			/* W. Australia */
	{"awst", TZ, 48},			/* W. Australia */
	{DB_C, ADBC, BC},			/* "bc" for years < 0 */
	{"bst", TZ, 6},				/* British Summer Time */
	{"bt", TZ, 18},				/* Baghdad Time */
	{"cadt", DTZ, 63},			/* Central Australian DST */
	{"cast", TZ, 57},			/* Central Australian ST */
	{"cat", TZ, NEG(60)},		/* Central Alaska Time */
	{"cct", TZ, 48},			/* China Coast */
	{"cdt", DTZ, NEG(30)},		/* Central Daylight Time */
	{"cet", TZ, 6},				/* Central European Time */
	{"cetdst", DTZ, 12},		/* Central European Dayl.Time */
#if USE_AUSTRALIAN_RULES
	{"cst", TZ, 63},			/* Australia Eastern Std Time */
#else
	{"cst", TZ, NEG(36)},		/* Central Standard Time */
#endif
	{DCURRENT, RESERV, DTK_CURRENT},	/* "current" is always now */
	{"dec", MONTH, 12},
	{"december", MONTH, 12},
	{"dnt", TZ, 6},				/* Dansk Normal Tid */
	{"dow", RESERV, DTK_DOW},	/* day of week */
	{"doy", RESERV, DTK_DOY},	/* day of year */
	{"dst", DTZMOD, 6},
	{"east", TZ, 60},			/* East Australian Std Time */
	{"edt", DTZ, NEG(24)},		/* Eastern Daylight Time */
	{"eet", TZ, 12},			/* East. Europe, USSR Zone 1 */
	{"eetdst", DTZ, 18},		/* Eastern Europe */
	{EPOCH, RESERV, DTK_EPOCH}, /* "epoch" reserved for system epoch time */
#if USE_AUSTRALIAN_RULES
	{"est", TZ, 60},			/* Australia Eastern Std Time */
#else
	{"est", TZ, NEG(30)},		/* Eastern Standard Time */
#endif
	{"feb", MONTH, 2},
	{"february", MONTH, 2},
	{"fri", DOW, 5},
	{"friday", DOW, 5},
	{"fst", TZ, 6},				/* French Summer Time */
	{"fwt", DTZ, 12},			/* French Winter Time  */
	{"gmt", TZ, 0},				/* Greenwish Mean Time */
	{"gst", TZ, 60},			/* Guam Std Time, USSR Zone 9 */
	{"hdt", DTZ, NEG(54)},		/* Hawaii/Alaska */
	{"hmt", DTZ, 18},			/* Hellas ? ? */
	{"hst", TZ, NEG(60)},		/* Hawaii Std Time */
	{"idle", TZ, 72},			/* Intl. Date Line, East */
	{"idlw", TZ, NEG(72)},		/* Intl. Date Line, West */
	{LATE, RESERV, DTK_LATE},	/* "infinity" reserved for "late time" */
	{INVALID, RESERV, DTK_INVALID},
	/* "invalid" reserved for invalid time */
	{"ist", TZ, 12},			/* Israel */
	{"it", TZ, 21},				/* Iran Time */
	{"jan", MONTH, 1},
	{"january", MONTH, 1},
	{"jst", TZ, 54},			/* Japan Std Time,USSR Zone 8 */
	{"jt", TZ, 45},				/* Java Time */
	{"jul", MONTH, 7},
	{"july", MONTH, 7},
	{"jun", MONTH, 6},
	{"june", MONTH, 6},
	{"kst", TZ, 54},			/* Korea Standard Time */
	{"ligt", TZ, 60},			/* From Melbourne, Australia */
	{"mar", MONTH, 3},
	{"march", MONTH, 3},
	{"may", MONTH, 5},
	{"mdt", DTZ, NEG(36)},		/* Mountain Daylight Time */
	{"mest", DTZ, 12},			/* Middle Europe Summer Time */
	{"met", TZ, 6},				/* Middle Europe Time */
	{"metdst", DTZ, 12},		/* Middle Europe Daylight Time */
	{"mewt", TZ, 6},			/* Middle Europe Winter Time */
	{"mez", TZ, 6},				/* Middle Europe Zone */
	{"mon", DOW, 1},
	{"monday", DOW, 1},
	{"mst", TZ, NEG(42)},		/* Mountain Standard Time */
	{"mt", TZ, 51},				/* Moluccas Time */
	{"ndt", DTZ, NEG(15)},		/* Nfld. Daylight Time */
	{"nft", TZ, NEG(21)},		/* Newfoundland Standard Time */
	{"nor", TZ, 6},				/* Norway Standard Time */
	{"nov", MONTH, 11},
	{"november", MONTH, 11},
	{NOW, RESERV, DTK_NOW},		/* current transaction time */
	{"nst", TZ, NEG(21)},		/* Nfld. Standard Time */
	{"nt", TZ, NEG(66)},		/* Nome Time */
	{"nzdt", DTZ, 78},			/* New Zealand Daylight Time */
	{"nzst", TZ, 72},			/* New Zealand Standard Time */
	{"nzt", TZ, 72},			/* New Zealand Time */
	{"oct", MONTH, 10},
	{"october", MONTH, 10},
	{"on", IGNORE, 0},			/* "on" (throwaway) */
	{"pdt", DTZ, NEG(42)},		/* Pacific Daylight Time */
	{"pm", AMPM, PM},
	{"pst", TZ, NEG(48)},		/* Pacific Standard Time */
	{"sadt", DTZ, 63},			/* S. Australian Dayl. Time */
	{"sast", TZ, 57},			/* South Australian Std Time */
	{"sat", DOW, 6},
	{"saturday", DOW, 6},
	{"sep", MONTH, 9},
	{"sept", MONTH, 9},
	{"september", MONTH, 9},
	{"set", TZ, NEG(6)},		/* Seychelles Time ?? */
	{"sst", DTZ, 12},			/* Swedish Summer Time */
	{"sun", DOW, 0},
	{"sunday", DOW, 0},
	{"swt", TZ, 6},				/* Swedish Winter Time	*/
	{"thu", DOW, 4},
	{"thur", DOW, 4},
	{"thurs", DOW, 4},
	{"thursday", DOW, 4},
	{TODAY, RESERV, DTK_TODAY}, /* midnight */
	{TOMORROW, RESERV, DTK_TOMORROW},	/* tomorrow midnight */
	{"tue", DOW, 2},
	{"tues", DOW, 2},
	{"tuesday", DOW, 2},
	{"undefined", RESERV, DTK_INVALID}, /* "undefined" pre-v6.1 invalid
										 * time */
	{"ut", TZ, 0},
	{"utc", TZ, 0},
	{"wadt", DTZ, 48},			/* West Australian DST */
	{"wast", TZ, 42},			/* West Australian Std Time */
	{"wat", TZ, NEG(6)},		/* West Africa Time */
	{"wdt", DTZ, 54},			/* West Australian DST */
	{"wed", DOW, 3},
	{"wednesday", DOW, 3},
	{"weds", DOW, 3},
	{"wet", TZ, 0},				/* Western Europe */
	{"wetdst", DTZ, 6},			/* Western Europe */
	{"wst", TZ, 48},			/* West Australian Std Time */
	{"ydt", DTZ, NEG(48)},		/* Yukon Daylight Time */
	{YESTERDAY, RESERV, DTK_YESTERDAY}, /* yesterday midnight */
	{"yst", TZ, NEG(54)},		/* Yukon Standard Time */
	{"zp4", TZ, NEG(24)},		/* GMT +4  hours. */
	{"zp5", TZ, NEG(30)},		/* GMT +5  hours. */
	{"zp6", TZ, NEG(36)},		/* GMT +6  hours. */
	{"z", RESERV, DTK_ZULU},	/* 00:00:00 */
	{ZULU, RESERV, DTK_ZULU},	/* 00:00:00 */
};

static unsigned int szdatetktbl = sizeof datetktbl / sizeof datetktbl[0];

static datetkn deltatktbl[] = {
/*		text			token	lexval */
	{"@", IGNORE, 0},			/* postgres relative time prefix */
	{DAGO, AGO, 0},				/* "ago" indicates negative time offset */
	{"c", UNITS, DTK_CENTURY},	/* "century" relative time units */
	{"cent", UNITS, DTK_CENTURY},		/* "century" relative time units */
	{"centuries", UNITS, DTK_CENTURY},	/* "centuries" relative time units */
	{DCENTURY, UNITS, DTK_CENTURY},		/* "century" relative time units */
	{"d", UNITS, DTK_DAY},		/* "day" relative time units */
	{DDAY, UNITS, DTK_DAY},		/* "day" relative time units */
	{"days", UNITS, DTK_DAY},	/* "days" relative time units */
	{"dec", UNITS, DTK_DECADE}, /* "decade" relative time units */
	{"decs", UNITS, DTK_DECADE},/* "decades" relative time units */
	{DDECADE, UNITS, DTK_DECADE},		/* "decade" relative time units */
	{"decades", UNITS, DTK_DECADE},		/* "decades" relative time units */
	{"h", UNITS, DTK_HOUR},		/* "hour" relative time units */
	{DHOUR, UNITS, DTK_HOUR},	/* "hour" relative time units */
	{"hours", UNITS, DTK_HOUR}, /* "hours" relative time units */
	{"hr", UNITS, DTK_HOUR},	/* "hour" relative time units */
	{"hrs", UNITS, DTK_HOUR},	/* "hours" relative time units */
	{INVALID, RESERV, DTK_INVALID},		/* "invalid" reserved for invalid
										 * time */
	{"m", UNITS, DTK_MINUTE},	/* "minute" relative time units */
	{"microsecon", UNITS, DTK_MICROSEC},		/* "microsecond" relative
												 * time units */
	{"mil", UNITS, DTK_MILLENIUM},		/* "millenium" relative time units */
	{"mils", UNITS, DTK_MILLENIUM},		/* "millenia" relative time units */
	{"millenia", UNITS, DTK_MILLENIUM}, /* "millenia" relative time units */
	{DMILLENIUM, UNITS, DTK_MILLENIUM}, /* "millenium" relative time units */
	{"millisecon", UNITS, DTK_MILLISEC},		/* relative time units */
	{"min", UNITS, DTK_MINUTE}, /* "minute" relative time units */
	{"mins", UNITS, DTK_MINUTE},/* "minutes" relative time units */
	{"mins", UNITS, DTK_MINUTE},/* "minutes" relative time units */
	{DMINUTE, UNITS, DTK_MINUTE},		/* "minute" relative time units */
	{"minutes", UNITS, DTK_MINUTE},		/* "minutes" relative time units */
	{"mon", UNITS, DTK_MONTH},	/* "months" relative time units */
	{"mons", UNITS, DTK_MONTH}, /* "months" relative time units */
	{DMONTH, UNITS, DTK_MONTH}, /* "month" relative time units */
	{"months", UNITS, DTK_MONTH},
	{"ms", UNITS, DTK_MILLISEC},
	{"msec", UNITS, DTK_MILLISEC},
	{DMILLISEC, UNITS, DTK_MILLISEC},
	{"mseconds", UNITS, DTK_MILLISEC},
	{"msecs", UNITS, DTK_MILLISEC},
	{"qtr", UNITS, DTK_QUARTER},/* "quarter" relative time */
	{DQUARTER, UNITS, DTK_QUARTER},		/* "quarter" relative time */
	{"reltime", IGNORE, 0},		/* for pre-v6.1 "Undefined Reltime" */
	{"s", UNITS, DTK_SECOND},
	{"sec", UNITS, DTK_SECOND},
	{DSECOND, UNITS, DTK_SECOND},
	{"seconds", UNITS, DTK_SECOND},
	{"secs", UNITS, DTK_SECOND},
	{DTIMEZONE, UNITS, DTK_TZ}, /* "timezone" time offset */
	{"tz", UNITS, DTK_TZ},		/* "timezone" time offset */
	{"tz_hour", UNITS, DTK_TZ_HOUR},	/* timezone hour units */
	{"tz_minute", UNITS, DTK_TZ_MINUTE},		/* timezone minutes units */
	{"undefined", RESERV, DTK_INVALID}, /* pre-v6.1 invalid time */
	{"us", UNITS, DTK_MICROSEC},/* "microsecond" relative time units */
	{"usec", UNITS, DTK_MICROSEC},		/* "microsecond" relative time
										 * units */
	{DMICROSEC, UNITS, DTK_MICROSEC},	/* "microsecond" relative time
										 * units */
	{"useconds", UNITS, DTK_MICROSEC},	/* "microseconds" relative time
										 * units */
	{"usecs", UNITS, DTK_MICROSEC},		/* "microseconds" relative time
										 * units */
	{"w", UNITS, DTK_WEEK},		/* "week" relative time units */
	{DWEEK, UNITS, DTK_WEEK},	/* "week" relative time units */
	{"weeks", UNITS, DTK_WEEK}, /* "weeks" relative time units */
	{"y", UNITS, DTK_YEAR},		/* "year" relative time units */
	{DYEAR, UNITS, DTK_YEAR},	/* "year" relative time units */
	{"years", UNITS, DTK_YEAR}, /* "years" relative time units */
	{"yr", UNITS, DTK_YEAR},	/* "year" relative time units */
	{"yrs", UNITS, DTK_YEAR},	/* "years" relative time units */
};

static unsigned int szdeltatktbl = sizeof deltatktbl / sizeof deltatktbl[0];

#if USE_DATE_CACHE
datetkn    *datecache[MAXDATEFIELDS] = {NULL};

datetkn    *deltacache[MAXDATEFIELDS] = {NULL};

#endif


/*
 * Calendar time to Julian date conversions.
 * Julian date is commonly used in astronomical applications,
 *	since it is numerically accurate and computationally simple.
 * The algorithms here will accurately convert between Julian day
 *	and calendar date for all non-negative Julian days
 *	(i.e. from Nov 23, -4713 on).
 *
 * Ref: Explanatory Supplement to the Astronomical Almanac, 1992.
 *	University Science Books, 20 Edgehill Rd. Mill Valley CA 94941.
 *
 * Use the algorithm by Henry Fliegel, a former NASA/JPL colleague
 *	now at Aerospace Corp. (hi, Henry!)
 *
 * These routines will be used by other date/time packages - tgl 97/02/25
 */

int
date2j(int y, int m, int d)
{
	int			m12 = (m - 14) / 12;

	return ((1461 * (y + 4800 + m12)) / 4 + (367 * (m - 2 - 12 * (m12))) / 12
			- (3 * ((y + 4900 + m12) / 100)) / 4 + d - 32075);
}	/* date2j() */

void
j2date(int jd, int *year, int *month, int *day)
{
	int			j,
				y,
				m,
				d;

	int			i,
				l,
				n;

	l = jd + 68569;
	n = (4 * l) / 146097;
	l -= (146097 * n + 3) / 4;
	i = (4000 * (l + 1)) / 1461001;
	l += 31 - (1461 * i) / 4;
	j = (80 * l) / 2447;
	d = l - (2447 * j) / 80;
	l = j / 11;
	m = (j + 2) - (12 * l);
	y = 100 * (n - 49) + i + l;

	*year = y;
	*month = m;
	*day = d;
	return;
}	/* j2date() */

int
j2day(int date)
{
	int			day;

	day = (date + 1) % 7;

	return day;
}	/* j2day() */


/*
 * parse and convert date in timestr (the normal interface)
 *
 * Returns the number of seconds since epoch (J2000)
 */

/* ParseDateTime()
 * Break string into tokens based on a date/time context.
 */
int
ParseDateTime(char *timestr, char *lowstr,
			  char **field, int *ftype, int maxfields, int *numfields)
{
	int			nf = 0;
	char	   *cp = timestr;
	char	   *lp = lowstr;

	/* outer loop through fields */
	while (*cp != '\0')
	{
		field[nf] = lp;

		/* leading digit? then date or time */
		if (isdigit(*cp) || (*cp == '.'))
		{
			*lp++ = *cp++;
			while (isdigit(*cp))
				*lp++ = *cp++;
			/* time field? */
			if (*cp == ':')
			{
				ftype[nf] = DTK_TIME;
				while (isdigit(*cp) || (*cp == ':') || (*cp == '.'))
					*lp++ = *cp++;

			}
			/* date field? allow embedded text month */
			else if ((*cp == '-') || (*cp == '/') || (*cp == '.'))
			{
				ftype[nf] = DTK_DATE;
				while (isalnum(*cp) || (*cp == '-') || (*cp == '/') || (*cp == '.'))
					*lp++ = tolower(*cp++);

			}

			/*
			 * otherwise, number only and will determine year, month, or
			 * day later
			 */
			else
				ftype[nf] = DTK_NUMBER;

		}

		/*
		 * text? then date string, month, day of week, special, or
		 * timezone
		 */
		else if (isalpha(*cp))
		{
			ftype[nf] = DTK_STRING;
			*lp++ = tolower(*cp++);
			while (isalpha(*cp))
				*lp++ = tolower(*cp++);

			/* Full date string with leading text month?
			 * Could also be a POSIX time zone...
			 */
			if ((*cp == '-') || (*cp == '/') || (*cp == '.'))
			{
#if 0
				/*
				 * special case of Posix timezone "GMT-0800"
				 * Note that other sign (e.g. "GMT+0800"
				 * is recognized as two separate fields and handled later.
				 * XXX There is no room for a delimiter between
				 * the "GMT" and the "-0800", so we are going to just swallow the "GMT".
				 * But this leads to other troubles with the definition of signs,
				 * so we have to flip
				 * - thomas 2000-02-06
				 */
				if ((*cp == '-') && isdigit(*(cp+1))
					&& (strncmp(field[nf], "gmt", 3) == 0))
				{
					*cp = '+';
					continue;
				}
#endif

				ftype[nf] = DTK_DATE;
				while (isdigit(*cp) || (*cp == '-') || (*cp == '/') || (*cp == '.'))
					*lp++ = tolower(*cp++);
			}

			/* skip leading spaces */
		}
		else if (isspace(*cp))
		{
			cp++;
			continue;

			/* sign? then special or numeric timezone */
		}
		else if ((*cp == '+') || (*cp == '-'))
		{
			*lp++ = *cp++;
			/* soak up leading whitespace */
			while (isspace(*cp))
				cp++;
			/* numeric timezone? */
			if (isdigit(*cp))
			{
				ftype[nf] = DTK_TZ;
				*lp++ = *cp++;
				while (isdigit(*cp) || (*cp == ':'))
					*lp++ = *cp++;

				/* special? */
			}
			else if (isalpha(*cp))
			{
				ftype[nf] = DTK_SPECIAL;
				*lp++ = tolower(*cp++);
				while (isalpha(*cp))
					*lp++ = tolower(*cp++);

				/* otherwise something wrong... */
			}
			else
				return -1;

			/* ignore punctuation but use as delimiter */
		}
		else if (ispunct(*cp))
		{
			cp++;
			continue;

		}
		else
			return -1;

		/* force in a delimiter */
		*lp++ = '\0';
		nf++;
		if (nf > MAXDATEFIELDS)
			return -1;
	}

	*numfields = nf;

	return 0;
}	/* ParseDateTime() */


/* DecodeDateTime()
 * Interpret previously parsed fields for general date and time.
 * Return 0 if full date, 1 if only time, and -1 if problems.
 *		External format(s):
 *				"<weekday> <month>-<day>-<year> <hour>:<minute>:<second>"
 *				"Fri Feb-7-1997 15:23:27"
 *				"Feb-7-1997 15:23:27"
 *				"2-7-1997 15:23:27"
 *				"1997-2-7 15:23:27"
 *				"1997.038 15:23:27"		(day of year 1-366)
 *		Also supports input in compact time:
 *				"970207 152327"
 *				"97038 152327"
 *
 * Use the system-provided functions to get the current time zone
 *	if not specified in the input string.
 * If the date is outside the time_t system-supported time range,
 *	then assume GMT time zone. - tgl 97/05/27
 */
int
DecodeDateTime(char **field, int *ftype, int nf,
			   int *dtype, struct tm * tm, double *fsec, int *tzp)
{
	int			fmask = 0,
				tmask,
				type;
	int			i;
	int			flen,
				val;
	int			mer = HR24;
	int			haveTextMonth = FALSE;
	int			is2digits = FALSE;
	int			bc = FALSE;

	*dtype = DTK_DATE;
	tm->tm_hour = 0;
	tm->tm_min = 0;
	tm->tm_sec = 0;
	*fsec = 0;
	tm->tm_isdst = -1;	/* don't know daylight savings time status apriori */
	if (tzp != NULL)
		*tzp = 0;

	for (i = 0; i < nf; i++)
	{
		switch (ftype[i])
		{
			case DTK_DATE:
				/* Already have a date?
				 * Then this might be a POSIX time zone
				 *  with an embedded dash (e.g. "PST-3" == "EST")
				 * - thomas 2000-03-15
				 */
				if ((fmask & DTK_DATE_M) == DTK_DATE_M)
				{
					if ((tzp == NULL)
						|| (DecodePosixTimezone(field[i], tzp) != 0))
						return -1;

					ftype[i] = DTK_TZ;
					tmask = DTK_M(TZ);
				}
				else if (DecodeDate(field[i], fmask, &tmask, tm) != 0)
					return -1;
				break;

			case DTK_TIME:
				if (DecodeTime(field[i], fmask, &tmask, tm, fsec) != 0)
					return -1;

				/*
				 * check upper limit on hours; other limits checked in
				 * DecodeTime()
				 */
				if (tm->tm_hour > 23)
					return -1;
				break;

			case DTK_TZ:
				if (tzp == NULL)
					return -1;

				{
					int tz;

					if (DecodeTimezone(field[i], &tz) != 0)
						return -1;

					/* Already have a time zone?
					 * Then maybe this is the second field of a POSIX time:
					 *  EST+3 (equivalent to PST)
					 */
					if ((i > 0) && ((fmask & DTK_M(TZ)) != 0)
						&& (ftype[i-1] == DTK_TZ) && (isalpha(*field[i-1])))
					{
						*tzp -= tz;
						tmask = 0;
					}
					else
					{
						*tzp = tz;
						tmask = DTK_M(TZ);
					}
				}
				break;

			case DTK_NUMBER:
				flen = strlen(field[i]);

				/*
				 * long numeric string and either no date or no time read
				 * yet? then interpret as a concatenated date or time...
				 */
				if ((flen > 4) && !((fmask & DTK_DATE_M) && (fmask & DTK_TIME_M)))
				{
					if (DecodeNumberField(flen, field[i], fmask, &tmask, tm, fsec, &is2digits) != 0)
						return -1;

				}
				/* otherwise it is a single date/time field... */
				else
				{
					if (DecodeNumber(flen, field[i], fmask, &tmask, tm, fsec, &is2digits) != 0)
						return -1;
				}
				break;

			case DTK_STRING:
			case DTK_SPECIAL:
				type = DecodeSpecial(i, field[i], &val);
				if (type == IGNORE)
					continue;

				tmask = DTK_M(type);
				switch (type)
				{
					case RESERV:
						switch (val)
						{
							case DTK_NOW:
								tmask = (DTK_DATE_M | DTK_TIME_M | DTK_M(TZ));
								*dtype = DTK_DATE;
								GetCurrentTime(tm);
								if (tzp != NULL)
									*tzp = CTimeZone;
								break;

							case DTK_YESTERDAY:
								tmask = DTK_DATE_M;
								*dtype = DTK_DATE;
								GetCurrentTime(tm);
								j2date((date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - 1),
								&tm->tm_year, &tm->tm_mon, &tm->tm_mday);
								tm->tm_hour = 0;
								tm->tm_min = 0;
								tm->tm_sec = 0;
								break;

							case DTK_TODAY:
								tmask = DTK_DATE_M;
								*dtype = DTK_DATE;
								GetCurrentTime(tm);
								tm->tm_hour = 0;
								tm->tm_min = 0;
								tm->tm_sec = 0;
								break;

							case DTK_TOMORROW:
								tmask = DTK_DATE_M;
								*dtype = DTK_DATE;
								GetCurrentTime(tm);
								j2date((date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) + 1),
								&tm->tm_year, &tm->tm_mon, &tm->tm_mday);
								tm->tm_hour = 0;
								tm->tm_min = 0;
								tm->tm_sec = 0;
								break;

							case DTK_ZULU:
								tmask = (DTK_TIME_M | DTK_M(TZ));
								*dtype = DTK_DATE;
								tm->tm_hour = 0;
								tm->tm_min = 0;
								tm->tm_sec = 0;
								if (tzp != NULL)
									*tzp = 0;
								break;

							default:
								*dtype = val;
						}

						break;

					case MONTH:
						/*
						 * already have a (numeric) month? then see if we
						 * can substitute...
						 */
						if ((fmask & DTK_M(MONTH)) && (!haveTextMonth)
							&& (!(fmask & DTK_M(DAY)))
							&& ((tm->tm_mon >= 1) && (tm->tm_mon <= 31)))
						{
							tm->tm_mday = tm->tm_mon;
							tmask = DTK_M(DAY);
						}
						haveTextMonth = TRUE;
						tm->tm_mon = val;
						break;

					case DTZMOD:

						/*
						 * daylight savings time modifier (solves "MET
						 * DST" syntax)
						 */
						tmask |= DTK_M(DTZ);
						tm->tm_isdst = 1;
						if (tzp == NULL)
							return -1;
						*tzp += val * 60;
						break;

					case DTZ:

						/*
						 * set mask for TZ here _or_ check for DTZ later
						 * when getting default timezone
						 */
						tmask |= DTK_M(TZ);
						tm->tm_isdst = 1;
						if (tzp == NULL)
							return -1;
						*tzp = val * 60;
						ftype[i] = DTK_TZ;
						break;

					case TZ:
						tm->tm_isdst = 0;
						if (tzp == NULL)
							return -1;
						*tzp = val * 60;
						ftype[i] = DTK_TZ;
						break;

					case IGNORE:
						break;

					case AMPM:
						mer = val;
						break;

					case ADBC:
						bc = (val == BC);
						break;

					case DOW:
						tm->tm_wday = val;
						break;

					default:
						return -1;
				}
				break;

			default:
				return -1;
		}

		if (tmask & fmask)
			return -1;
		fmask |= tmask;
	}

	/* there is no year zero in AD/BC notation; i.e. "1 BC" == year 0 */
	if (bc)
	{
		if (tm->tm_year > 0)
			tm->tm_year = -(tm->tm_year - 1);
		else
			elog(ERROR, "Inconsistant use of year %04d and 'BC'", tm->tm_year);
	}
	else if (is2digits)
	{
		if (tm->tm_year < 70)
			tm->tm_year += 2000;
		else if (tm->tm_year < 100)
			tm->tm_year += 1900;
	}

	if ((mer != HR24) && (tm->tm_hour > 12))
		return -1;
	if ((mer == AM) && (tm->tm_hour == 12))
		tm->tm_hour = 0;
	else if ((mer == PM) && (tm->tm_hour != 12))
		tm->tm_hour += 12;

	/* do additional checking for full date specs... */
	if (*dtype == DTK_DATE)
	{
		if ((fmask & DTK_DATE_M) != DTK_DATE_M)
			return ((fmask & DTK_TIME_M) == DTK_TIME_M) ? 1 : -1;

		/*
		 * check for valid day of month, now that we know for sure the
		 * month and year...
		 */
		if ((tm->tm_mday < 1)
		 || (tm->tm_mday > day_tab[isleap(tm->tm_year)][tm->tm_mon - 1]))
			return -1;

		/* timezone not specified? then find local timezone if possible */
		if (((fmask & DTK_DATE_M) == DTK_DATE_M)
			&& (tzp != NULL) && (!(fmask & DTK_M(TZ))))
		{

			/*
			 * daylight savings time modifier but no standard timezone?
			 * then error
			 */
			if (fmask & DTK_M(DTZMOD))
				return -1;

			if (IS_VALID_UTIME(tm->tm_year, tm->tm_mon, tm->tm_mday))
			{
#ifdef USE_POSIX_TIME
				tm->tm_year -= 1900;
				tm->tm_mon -= 1;
				tm->tm_isdst = -1;
				mktime(tm);
				tm->tm_year += 1900;
				tm->tm_mon += 1;

#if defined(HAVE_TM_ZONE)
				*tzp = -(tm->tm_gmtoff);	/* tm_gmtoff is Sun/DEC-ism */
#elif defined(HAVE_INT_TIMEZONE)
#ifdef __CYGWIN__
				*tzp = ((tm->tm_isdst > 0) ? (_timezone - 3600) : _timezone);
#else
				*tzp = ((tm->tm_isdst > 0) ? (timezone - 3600) : timezone);
#endif
#else
#error USE_POSIX_TIME is defined but neither HAVE_TM_ZONE or HAVE_INT_TIMEZONE are defined
#endif

#else							/* !USE_POSIX_TIME */
				*tzp = CTimeZone;
#endif
			}
			else
			{
				tm->tm_isdst = 0;
				*tzp = 0;
			}
		}
	}

	return 0;
}	/* DecodeDateTime() */


/* DecodeTimeOnly()
 * Interpret parsed string as time fields only.
 * Note that support for time zone is here for
 * SQL92 TIME WITH TIME ZONE, but it reveals
 * bogosity with SQL92 date/time standards, since
 * we must infer a time zone from current time.
 * XXX Later, we should probably support
 * SET TIME ZONE <integer>
 * which of course is a screwed up convention.
 * - thomas 2000-03-10
 */
int
DecodeTimeOnly(char **field, int *ftype, int nf,
			   int *dtype, struct tm * tm, double *fsec, int *tzp)
{
	int			fmask,
				tmask,
				type;
	int			i;
	int			flen,
				val;
	int			is2digits = FALSE;
	int			mer = HR24;

	*dtype = DTK_TIME;
	tm->tm_hour = 0;
	tm->tm_min = 0;
	tm->tm_sec = 0;
	*fsec = 0;
	tm->tm_isdst = -1;	/* don't know daylight savings time status apriori */
	if (tzp != NULL)
		*tzp = 0;

	fmask = DTK_DATE_M;

	for (i = 0; i < nf; i++)
	{
		switch (ftype[i])
		{
			case DTK_DATE:
				/* This might be a POSIX time zone
				 *  with an embedded dash (e.g. "PST-3" == "EST")
				 * - thomas 2000-03-15
				 */
				if ((tzp == NULL)
					|| (DecodePosixTimezone(field[i], tzp) != 0))
					return -1;

				ftype[i] = DTK_TZ;
				tmask = DTK_M(TZ);
				break;

			case DTK_TIME:
				if (DecodeTime(field[i], fmask, &tmask, tm, fsec) != 0)
					return -1;
				break;

			case DTK_TZ:
				if (tzp == NULL)
					return -1;

				{
					int tz;

					if (DecodeTimezone(field[i], &tz) != 0)
						return -1;

					/* Already have a time zone?
					 * Then maybe this is the second field of a POSIX time:
					 *  EST+3 (equivalent to PST)
					 */
					if ((i > 0) && ((fmask & DTK_M(TZ)) != 0)
						&& (ftype[i-1] == DTK_TZ) && (isalpha(*field[i-1])))
					{
						*tzp -= tz;
						tmask = 0;
					}
					else
					{
						*tzp = tz;
						tmask = DTK_M(TZ);
					}
				}
				break;

			case DTK_NUMBER:
				flen = strlen(field[i]);

				if (DecodeNumberField(flen, field[i], fmask, &tmask, tm, fsec, &is2digits) != 0)
					return -1;
				break;

			case DTK_STRING:
			case DTK_SPECIAL:
				type = DecodeSpecial(i, field[i], &val);
				if (type == IGNORE)
					continue;

				tmask = DTK_M(type);
				switch (type)
				{
					case RESERV:
						switch (val)
						{
							case DTK_NOW:
								tmask = DTK_TIME_M;
								*dtype = DTK_TIME;
								GetCurrentTime(tm);
								break;

							case DTK_ZULU:
								tmask = (DTK_TIME_M | DTK_M(TZ));
								*dtype = DTK_TIME;
								tm->tm_hour = 0;
								tm->tm_min = 0;
								tm->tm_sec = 0;
								tm->tm_isdst = 0;
								break;

							default:
								return -1;
						}

						break;

					case DTZMOD:

						/*
						 * daylight savings time modifier (solves "MET
						 * DST" syntax)
						 */
						tmask |= DTK_M(DTZ);
						tm->tm_isdst = 1;
						if (tzp == NULL)
							return -1;
						*tzp += val * 60;
						break;

					case DTZ:

						/*
						 * set mask for TZ here _or_ check for DTZ later
						 * when getting default timezone
						 */
						tmask |= DTK_M(TZ);
						tm->tm_isdst = 1;
						if (tzp == NULL)
							return -1;
						*tzp = val * 60;
						ftype[i] = DTK_TZ;
						break;

					case TZ:
						tm->tm_isdst = 0;
						if (tzp == NULL)
							return -1;
						*tzp = val * 60;
						ftype[i] = DTK_TZ;
						break;

					case IGNORE:
						break;

					case AMPM:
						mer = val;
						break;

					default:
						return -1;
				}
				break;

			default:
				return -1;
		}

		if (tmask & fmask)
			return -1;
		fmask |= tmask;
	}

	if ((mer != HR24) && (tm->tm_hour > 12))
		return -1;
	if ((mer == AM) && (tm->tm_hour == 12))
		tm->tm_hour = 0;
	else if ((mer == PM) && (tm->tm_hour != 12))
		tm->tm_hour += 12;

	if (((tm->tm_hour < 0) || (tm->tm_hour > 23))
		|| ((tm->tm_min < 0) || (tm->tm_min > 59))
		|| ((tm->tm_sec < 0) || ((tm->tm_sec + *fsec) >= 60)))
		return -1;

	if ((fmask & DTK_TIME_M) != DTK_TIME_M)
		return -1;

	/* timezone not specified? then find local timezone if possible */
	if ((tzp != NULL) && (!(fmask & DTK_M(TZ))))
	{
		struct tm tt, *tmp = &tt;

		/*
		 * daylight savings time modifier but no standard timezone?
		 * then error
		 */
		if (fmask & DTK_M(DTZMOD))
			return -1;

		GetCurrentTime(tmp);
		tmp->tm_hour = tm->tm_hour;
		tmp->tm_min = tm->tm_min;
		tmp->tm_sec = tm->tm_sec;

#ifdef USE_POSIX_TIME
		tmp->tm_isdst = -1;
		mktime(tmp);
		tm->tm_isdst = tmp->tm_isdst;

#if defined(HAVE_TM_ZONE)
		*tzp = -(tmp->tm_gmtoff);	/* tm_gmtoff is Sun/DEC-ism */
#elif defined(HAVE_INT_TIMEZONE)
#ifdef __CYGWIN__
		*tzp = ((tmp->tm_isdst > 0) ? (_timezone - 3600) : _timezone);
#else
		*tzp = ((tmp->tm_isdst > 0) ? (timezone - 3600) : timezone);
#endif
#else
#error USE_POSIX_TIME is defined but neither HAVE_TM_ZONE or HAVE_INT_TIMEZONE are defined
#endif

#else							/* !USE_POSIX_TIME */
		*tzp = CTimeZone;
#endif
	}

	return 0;
}	/* DecodeTimeOnly() */


/* DecodeDate()
 * Decode date string which includes delimiters.
 * Insist on a complete set of fields.
 */
int
DecodeDate(char *str, int fmask, int *tmask, struct tm * tm)
{
	double		fsec;

	int			nf = 0;
	int			i,
				len;
	int			bc = FALSE;
	int			is2digits = FALSE;
	int			type,
				val,
				dmask = 0;
	char	   *field[MAXDATEFIELDS];

	/* parse this string... */
	while ((*str != '\0') && (nf < MAXDATEFIELDS))
	{
		/* skip field separators */
		while (!isalnum(*str))
			str++;

		field[nf] = str;
		if (isdigit(*str))
		{
			while (isdigit(*str))
				str++;
		}
		else if (isalpha(*str))
		{
			while (isalpha(*str))
				str++;
		}

		if (*str != '\0')
			*str++ = '\0';
		nf++;
	}

#if 0
	/* don't allow too many fields */
	if (nf > 3)
		return -1;
#endif

	*tmask = 0;

	/* look first for text fields, since that will be unambiguous month */
	for (i = 0; i < nf; i++)
	{
		if (isalpha(*field[i]))
		{
			type = DecodeSpecial(i, field[i], &val);
			if (type == IGNORE)
				continue;

			dmask = DTK_M(type);
			switch (type)
			{
				case MONTH:
					tm->tm_mon = val;
					break;

				case ADBC:
					bc = (val == BC);
					break;

				default:
					return -1;
			}
			if (fmask & dmask)
				return -1;

			fmask |= dmask;
			*tmask |= dmask;

			/* mark this field as being completed */
			field[i] = NULL;
		}
	}

	/* now pick up remaining numeric fields */
	for (i = 0; i < nf; i++)
	{
		if (field[i] == NULL)
			continue;

		if ((len = strlen(field[i])) <= 0)
			return -1;

		if (DecodeNumber(len, field[i], fmask, &dmask, tm, &fsec, &is2digits) != 0)
			return -1;

		if (fmask & dmask)
			return -1;

		fmask |= dmask;
		*tmask |= dmask;
	}

	if ((fmask & ~(DTK_M(DOY) | DTK_M(TZ))) != DTK_DATE_M)
		return -1;

	/* there is no year zero in AD/BC notation; i.e. "1 BC" == year 0 */
	if (bc)
	{
		if (tm->tm_year > 0)
			tm->tm_year = -(tm->tm_year - 1);
		else
			elog(ERROR, "Inconsistant use of year %04d and 'BC'", tm->tm_year);
	}
	else if (is2digits)
	{
		if (tm->tm_year < 70)
			tm->tm_year += 2000;
		else if (tm->tm_year < 100)
			tm->tm_year += 1900;
	}

	return 0;
}	/* DecodeDate() */


/* DecodeTime()
 * Decode time string which includes delimiters.
 * Only check the lower limit on hours, since this same code
 *	can be used to represent time spans.
 */
int
DecodeTime(char *str, int fmask, int *tmask, struct tm * tm, double *fsec)
{
	char	   *cp;

	*tmask = DTK_TIME_M;

	tm->tm_hour = strtol(str, &cp, 10);
	if (*cp != ':')
		return -1;
	str = cp + 1;
	tm->tm_min = strtol(str, &cp, 10);
	if (*cp == '\0')
	{
		tm->tm_sec = 0;
		*fsec = 0;

	}
	else if (*cp != ':')
	{
		return -1;

	}
	else
	{
		str = cp + 1;
		tm->tm_sec = strtol(str, &cp, 10);
		if (*cp == '\0')
			*fsec = 0;
		else if (*cp == '.')
		{
			str = cp;
			*fsec = strtod(str, &cp);
			if (cp == str)
				return -1;
		}
		else
			return -1;
	}

	/* do a sanity check */
	if ((tm->tm_hour < 0)
		|| (tm->tm_min < 0) || (tm->tm_min > 59)
		|| (tm->tm_sec < 0) || (tm->tm_sec > 59))
		return -1;

	return 0;
}	/* DecodeTime() */


/* DecodeNumber()
 * Interpret numeric field as a date value in context.
 */
int
DecodeNumber(int flen, char *str, int fmask,
			 int *tmask, struct tm * tm, double *fsec, int *is2digits)
{
	int			val;
	char	   *cp;

	*tmask = 0;

	val = strtol(str, &cp, 10);
	if (cp == str)
		return -1;
	if (*cp == '.')
	{
		*fsec = strtod(cp, &cp);
		if (*cp != '\0')
			return -1;
	}

	/* Special case day of year? */
	if ((flen == 3) && (fmask & DTK_M(YEAR))
		&& ((val >= 1) && (val <= 366)))
	{
		*tmask = (DTK_M(DOY) | DTK_M(MONTH) | DTK_M(DAY));
		tm->tm_yday = val;
		j2date((date2j(tm->tm_year, 1, 1) + tm->tm_yday - 1),
			   &tm->tm_year, &tm->tm_mon, &tm->tm_mday);

	}

	/*
	 * Enough digits to be unequivocal year? Used to test for 4 digits or
	 * more, but we now test first for a three-digit doy so anything
	 * bigger than two digits had better be an explicit year. - thomas
	 * 1999-01-09
	 */
	else if (flen > 2)
	{
		*tmask = DTK_M(YEAR);

		/* already have a year? then see if we can substitute... */
		if ((fmask & DTK_M(YEAR)) && (!(fmask & DTK_M(DAY)))
			&& ((tm->tm_year >= 1) && (tm->tm_year <= 31)))
		{
			tm->tm_mday = tm->tm_year;
			*tmask = DTK_M(DAY);
		}

		tm->tm_year = val;
	}
	/* already have year? then could be month */
	else if ((fmask & DTK_M(YEAR)) && (!(fmask & DTK_M(MONTH)))
			 && ((val >= 1) && (val <= 12)))
	{
		*tmask = DTK_M(MONTH);
		tm->tm_mon = val;
		/* no year and EuroDates enabled? then could be day */
	}
	else if ((EuroDates || (fmask & DTK_M(MONTH)))
			 && (!(fmask & DTK_M(YEAR)) && !(fmask & DTK_M(DAY)))
			 && ((val >= 1) && (val <= 31)))
	{
		*tmask = DTK_M(DAY);
		tm->tm_mday = val;
	}
	else if ((!(fmask & DTK_M(MONTH)))
			 && ((val >= 1) && (val <= 12)))
	{
		*tmask = DTK_M(MONTH);
		tm->tm_mon = val;
	}
	else if ((!(fmask & DTK_M(DAY)))
			 && ((val >= 1) && (val <= 31)))
	{
		*tmask = DTK_M(DAY);
		tm->tm_mday = val;
	}
	else if (!(fmask & DTK_M(YEAR)))
	{
		*tmask = DTK_M(YEAR);
		tm->tm_year = val;

		/* adjust ONLY if exactly two digits... */
		*is2digits = (flen == 2);
	}
	else
		return -1;

	return 0;
}	/* DecodeNumber() */


/* DecodeNumberField()
 * Interpret numeric string as a concatenated date field.
 */
int
DecodeNumberField(int len, char *str, int fmask,
				int *tmask, struct tm * tm, double *fsec, int *is2digits)
{
	char	   *cp;

	/* yyyymmdd? */
	if (len == 8)
	{
		*tmask = DTK_DATE_M;

		tm->tm_mday = atoi(str + 6);
		*(str + 6) = '\0';
		tm->tm_mon = atoi(str + 4);
		*(str + 4) = '\0';
		tm->tm_year = atoi(str + 0);
		/* yymmdd or hhmmss? */
	}
	else if (len == 6)
	{
		if (fmask & DTK_DATE_M)
		{
			*tmask = DTK_TIME_M;
			tm->tm_sec = atoi(str + 4);
			*(str + 4) = '\0';
			tm->tm_min = atoi(str + 2);
			*(str + 2) = '\0';
			tm->tm_hour = atoi(str + 0);
		}
		else
		{
			*tmask = DTK_DATE_M;
			tm->tm_mday = atoi(str + 4);
			*(str + 4) = '\0';
			tm->tm_mon = atoi(str + 2);
			*(str + 2) = '\0';
			tm->tm_year = atoi(str + 0);
			*is2digits = TRUE;
		}

	}
	else if ((len == 5) && !(fmask & DTK_DATE_M))
	{
		*tmask = DTK_DATE_M;
		tm->tm_mday = atoi(str + 2);
		*(str + 2) = '\0';
		tm->tm_mon = 1;
		tm->tm_year = atoi(str + 0);
		*is2digits = TRUE;
	}
	else if (strchr(str, '.') != NULL)
	{
		*tmask = DTK_TIME_M;
		tm->tm_sec = strtod((str + 4), &cp);
		if (cp == (str + 4))
			return -1;
		if (*cp == '.')
			*fsec = strtod(cp, NULL);
		*(str + 4) = '\0';
		tm->tm_min = strtod((str + 2), &cp);
		*(str + 2) = '\0';
		tm->tm_hour = strtod((str + 0), &cp);

	}
	else
		return -1;

	return 0;
}	/* DecodeNumberField() */


/* DecodeTimezone()
 * Interpret string as a numeric timezone.
 */
int
DecodeTimezone(char *str, int *tzp)
{
	int			tz;
	int			hr,
				min;
	char	   *cp;
	int			len;

	/* assume leading character is "+" or "-" */
	hr = strtol((str + 1), &cp, 10);

	/* explicit delimiter? */
	if (*cp == ':')
	{
		min = strtol((cp + 1), &cp, 10);

		/* otherwise, might have run things together... */
	}
	else if ((*cp == '\0') && ((len = strlen(str)) > 3))
	{
		min = strtol((str + len - 2), &cp, 10);
		*(str + len - 2) = '\0';
		hr = strtol((str + 1), &cp, 10);

	}
	else
		min = 0;

	tz = (hr * 60 + min) * 60;
	if (*str == '-')
		tz = -tz;

	*tzp = -tz;
	return *cp != '\0';
}	/* DecodeTimezone() */


/* DecodePosixTimezone()
 * Interpret string as a POSIX-compatible timezone:
 *  PST-hh:mm
 *  PST+h
 * - thomas 2000-03-15
 */
static int
DecodePosixTimezone(char *str, int *tzp)
{
	int			val, tz;
	int			type;
	char	   *cp;
	char		delim;

	cp = str;
	while ((*cp != '\0') && isalpha(*cp))
		cp++;

	if (DecodeTimezone(cp, &tz) != 0)
		return -1;

	delim = *cp;
	*cp = '\0';
	type = DecodeSpecial(MAXDATEFIELDS-1, str, &val);
	*cp = delim;

	switch(type)
	{
		case DTZ:
		case TZ:
			*tzp = (val * 60) - tz;
			break;

		default:
			return -1;
	}

	return 0;
}	/* DecodePosixTimezone() */


/* DecodeSpecial()
 * Decode text string using lookup table.
 * Implement a cache lookup since it is likely that dates
 *	will be related in format.
 */
int
DecodeSpecial(int field, char *lowtoken, int *val)
{
	int			type;
	datetkn    *tp;

#if USE_DATE_CACHE
	if ((datecache[field] != NULL)
		&& (strncmp(lowtoken, datecache[field]->token, TOKMAXLEN) == 0))
		tp = datecache[field];
	else
	{
#endif
		tp = datebsearch(lowtoken, datetktbl, szdatetktbl);
#if USE_DATE_CACHE
	}
	datecache[field] = tp;
#endif
	if (tp == NULL)
	{
		type = IGNORE;
		*val = 0;
	}
	else
	{
		type = tp->type;
		switch (type)
		{
			case TZ:
			case DTZ:
			case DTZMOD:
				*val = FROMVAL(tp);
				break;

			default:
				*val = tp->value;
				break;
		}
	}

	return type;
}	/* DecodeSpecial() */


/* DecodeDateDelta()
 * Interpret previously parsed fields for general time interval.
 * Return 0 if decoded and -1 if problems.
 *
 * Allow "date" field DTK_DATE since this could be just
 *	an unsigned floating point number. - thomas 1997-11-16
 *
 * Allow ISO-style time span, with implicit units on number of days
 *	preceeding an hh:mm:ss field. - thomas 1998-04-30
 */
int
DecodeDateDelta(char **field, int *ftype, int nf, int *dtype, struct tm * tm, double *fsec)
{
	int			is_before = FALSE;

	char	   *cp;
	int			fmask = 0,
				tmask,
				type;
	int			i;
	int			flen,
				val;
	double		fval;
	double		sec;

	*dtype = DTK_DELTA;

	type = DTK_SECOND;
	tm->tm_year = 0;
	tm->tm_mon = 0;
	tm->tm_mday = 0;
	tm->tm_hour = 0;
	tm->tm_min = 0;
	tm->tm_sec = 0;
	*fsec = 0;

	/* read through list backwards to pick up units before values */
	for (i = nf - 1; i >= 0; i--)
	{
		switch (ftype[i])
		{
			case DTK_TIME:
				if (DecodeTime(field[i], fmask, &tmask, tm, fsec) != 0)
					return -1;
				type = DTK_DAY;
				break;

			case DTK_TZ:

				/*
				 * Timezone is a token with a leading sign character and
				 * otherwise the same as a non-signed numeric field
				 */
			case DTK_DATE:
			case DTK_NUMBER:
				val = strtol(field[i], &cp, 10);
				if (*cp == '.')
				{
					fval = strtod(cp, &cp);
					if (*cp != '\0')
						return -1;

					if (val < 0)
						fval = -(fval);
				}
				else if (*cp == '\0')
					fval = 0;
				else
					return -1;

				flen = strlen(field[i]);
				tmask = 0;		/* DTK_M(type); */

				switch (type)
				{
					case DTK_MICROSEC:
						*fsec += ((val + fval) * 1e-6);
						break;

					case DTK_MILLISEC:
						*fsec += ((val + fval) * 1e-3);
						break;

					case DTK_SECOND:
						tm->tm_sec += val;
						*fsec += fval;
						tmask = DTK_M(SECOND);
						break;

					case DTK_MINUTE:
						tm->tm_min += val;
						if (fval != 0)
							tm->tm_sec += (fval * 60);
						tmask = DTK_M(MINUTE);
						break;

					case DTK_HOUR:
						tm->tm_hour += val;
						if (fval != 0)
							tm->tm_sec += (fval * 3600);
						tmask = DTK_M(HOUR);
						break;

					case DTK_DAY:
						tm->tm_mday += val;
						if (fval != 0)
							tm->tm_sec += (fval * 86400);
						tmask = ((fmask & DTK_M(DAY)) ? 0 : DTK_M(DAY));
						break;

					case DTK_WEEK:
						tm->tm_mday += val * 7;
						if (fval != 0)
							tm->tm_sec += (fval * (7 * 86400));
						tmask = ((fmask & DTK_M(DAY)) ? 0 : DTK_M(DAY));
						break;

					case DTK_MONTH:
						tm->tm_mon += val;
						if (fval != 0)
							tm->tm_sec += (fval * (30 * 86400));
						tmask = DTK_M(MONTH);
						break;

					case DTK_YEAR:
						tm->tm_year += val;
						if (fval != 0)
							tm->tm_mon += (fval * 12);
						tmask = ((fmask & DTK_M(YEAR)) ? 0 : DTK_M(YEAR));
						break;

					case DTK_DECADE:
						tm->tm_year += val * 10;
						if (fval != 0)
							tm->tm_mon += (fval * 120);
						tmask = ((fmask & DTK_M(YEAR)) ? 0 : DTK_M(YEAR));
						break;

					case DTK_CENTURY:
						tm->tm_year += val * 100;
						if (fval != 0)
							tm->tm_mon += (fval * 1200);
						tmask = ((fmask & DTK_M(YEAR)) ? 0 : DTK_M(YEAR));
						break;

					case DTK_MILLENIUM:
						tm->tm_year += val * 1000;
						if (fval != 0)
							tm->tm_mon += (fval * 12000);
						tmask = ((fmask & DTK_M(YEAR)) ? 0 : DTK_M(YEAR));
						break;

					default:
						return -1;
				}
				break;

			case DTK_STRING:
			case DTK_SPECIAL:
				type = DecodeUnits(i, field[i], &val);
				if (type == IGNORE)
					continue;

				tmask = 0;		/* DTK_M(type); */
				switch (type)
				{
					case UNITS:
						type = val;
						break;

					case AGO:
						is_before = TRUE;
						type = val;
						break;

					case RESERV:
						tmask = (DTK_DATE_M || DTK_TIME_M);
						*dtype = val;
						break;

					default:
						return -1;
				}
				break;

			default:
				return -1;
		}

		if (tmask & fmask)
			return -1;
		fmask |= tmask;
	}

	if (*fsec != 0)
	{
		TMODULO(*fsec, sec, 1e0);
		tm->tm_sec += sec;
	}

	if (is_before)
	{
		*fsec = -(*fsec);
		tm->tm_sec = -(tm->tm_sec);
		tm->tm_min = -(tm->tm_min);
		tm->tm_hour = -(tm->tm_hour);
		tm->tm_mday = -(tm->tm_mday);
		tm->tm_mon = -(tm->tm_mon);
		tm->tm_year = -(tm->tm_year);
	}

	/* ensure that at least one time field has been found */
	return (fmask != 0) ? 0 : -1;
}	/* DecodeDateDelta() */


/* DecodeUnits()
 * Decode text string using lookup table.
 * This routine supports time interval decoding.
 */
int
DecodeUnits(int field, char *lowtoken, int *val)
{
	int			type;
	datetkn    *tp;

#if USE_DATE_CACHE
	if ((deltacache[field] != NULL)
		&& (strncmp(lowtoken, deltacache[field]->token, TOKMAXLEN) == 0))
		tp = deltacache[field];
	else
	{
#endif
		tp = datebsearch(lowtoken, deltatktbl, szdeltatktbl);
#if USE_DATE_CACHE
	}
	deltacache[field] = tp;
#endif
	if (tp == NULL)
	{
		type = IGNORE;
		*val = 0;
	}
	else
	{
		type = tp->type;
		if ((type == TZ) || (type == DTZ))
			*val = FROMVAL(tp);
		else
			*val = tp->value;
	}

	return type;
}	/* DecodeUnits() */


/* datebsearch()
 * Binary search -- from Knuth (6.2.1) Algorithm B.  Special case like this
 * is WAY faster than the generic bsearch().
 */
datetkn *
datebsearch(char *key, datetkn *base, unsigned int nel)
{
	datetkn    *last = base + nel - 1,
			   *position;
	int			result;

	while (last >= base)
	{
		position = base + ((last - base) >> 1);
		result = key[0] - position->token[0];
		if (result == 0)
		{
			result = strncmp(key, position->token, TOKMAXLEN);
			if (result == 0)
				return position;
		}
		if (result < 0)
			last = position - 1;
		else
			base = position + 1;
	}
	return NULL;
}


/* EncodeDateOnly()
 * Encode date as local time.
 */
int
EncodeDateOnly(struct tm * tm, int style, char *str)
{
	if ((tm->tm_mon < 1) || (tm->tm_mon > 12))
		return -1;

	switch (style)
	{
			/* compatible with ISO date formats */
		case USE_ISO_DATES:
			if (tm->tm_year > 0)
				sprintf(str, "%04d-%02d-%02d",
						tm->tm_year, tm->tm_mon, tm->tm_mday);
			else
				sprintf(str, "%04d-%02d-%02d %s",
					  -(tm->tm_year - 1), tm->tm_mon, tm->tm_mday, "BC");
			break;

			/* compatible with Oracle/Ingres date formats */
		case USE_SQL_DATES:
			if (EuroDates)
				sprintf(str, "%02d/%02d", tm->tm_mday, tm->tm_mon);
			else
				sprintf(str, "%02d/%02d", tm->tm_mon, tm->tm_mday);
			if (tm->tm_year > 0)
				sprintf((str + 5), "/%04d", tm->tm_year);
			else
				sprintf((str + 5), "/%04d %s", -(tm->tm_year - 1), "BC");
			break;

			/* German-style date format */
		case USE_GERMAN_DATES:
			sprintf(str, "%02d.%02d", tm->tm_mday, tm->tm_mon);
			if (tm->tm_year > 0)
				sprintf((str + 5), ".%04d", tm->tm_year);
			else
				sprintf((str + 5), ".%04d %s", -(tm->tm_year - 1), "BC");
			break;

			/* traditional date-only style for Postgres */
		case USE_POSTGRES_DATES:
		default:
			if (EuroDates)
				sprintf(str, "%02d-%02d", tm->tm_mday, tm->tm_mon);
			else
				sprintf(str, "%02d-%02d", tm->tm_mon, tm->tm_mday);
			if (tm->tm_year > 0)
				sprintf((str + 5), "-%04d", tm->tm_year);
			else
				sprintf((str + 5), "-%04d %s", -(tm->tm_year - 1), "BC");
			break;
	}

	return TRUE;
}	/* EncodeDateOnly() */


/* EncodeTimeOnly()
 * Encode time fields only.
 */
int
EncodeTimeOnly(struct tm * tm, double fsec, int *tzp, int style, char *str)
{
	double		sec;

	if ((tm->tm_hour < 0) || (tm->tm_hour > 24))
		return -1;

	sec = (tm->tm_sec + fsec);

	sprintf(str, "%02d:%02d:", tm->tm_hour, tm->tm_min);
	sprintf((str + 6), ((fsec != 0) ? "%05.2f" : "%02.0f"), sec);

	if (tzp != NULL)
	{
		int hour, min;

		hour = -(*tzp / 3600);
		min = ((abs(*tzp) / 60) % 60);
		sprintf((str + strlen(str)), ((min != 0) ? "%+03d:%02d" : "%+03d"), hour, min);
	}

	return TRUE;
}	/* EncodeTimeOnly() */


/* EncodeDateTime()
 * Encode date and time interpreted as local time.
 * Support several date styles:
 *	Postgres - day mon hh:mm:ss yyyy tz
 *	SQL - mm/dd/yyyy hh:mm:ss.ss tz
 *	ISO - yyyy-mm-dd hh:mm:ss+/-tz
 *	German - dd.mm/yyyy hh:mm:ss tz
 * Variants (affects order of month and day for Postgres and SQL styles):
 *	US - mm/dd/yyyy
 *	European - dd/mm/yyyy
 */
int
EncodeDateTime(struct tm * tm, double fsec, int *tzp, char **tzn, int style, char *str)
{
	int			day,
				hour,
				min;
	double		sec;

	if ((tm->tm_mon < 1) || (tm->tm_mon > 12))
		return -1;

	sec = (tm->tm_sec + fsec);

	switch (style)
	{
			/* compatible with ISO date formats */

		case USE_ISO_DATES:
			if (tm->tm_year > 0)
			{
				sprintf(str, "%04d-%02d-%02d %02d:%02d:",
						tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min);
				sprintf((str + 17), ((fsec != 0) ? "%05.2f" : "%02.0f"), sec);

				if ((*tzn != NULL) && (tm->tm_isdst >= 0))
				{
					if (tzp != NULL)
					{
						hour = -(*tzp / 3600);
						min = ((abs(*tzp) / 60) % 60);
					}
					else
					{
						hour = 0;
						min = 0;
					}
					sprintf((str + strlen(str)), ((min != 0) ? "%+03d:%02d" : "%+03d"), hour, min);
				}

			}
			else
			{
				if (tm->tm_hour || tm->tm_min)
					sprintf(str, "%04d-%02d-%02d %02d:%02d %s",
							-(tm->tm_year - 1), tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, "BC");
				else
					sprintf(str, "%04d-%02d-%02d %s",
					  -(tm->tm_year - 1), tm->tm_mon, tm->tm_mday, "BC");
			}
			break;

			/* compatible with Oracle/Ingres date formats */
		case USE_SQL_DATES:
			if (EuroDates)
				sprintf(str, "%02d/%02d", tm->tm_mday, tm->tm_mon);
			else
				sprintf(str, "%02d/%02d", tm->tm_mon, tm->tm_mday);

			if (tm->tm_year > 0)
			{
				sprintf((str + 5), "/%04d %02d:%02d:%05.2f",
						tm->tm_year, tm->tm_hour, tm->tm_min, sec);

				if ((*tzn != NULL) && (tm->tm_isdst >= 0))
				{
					strcpy((str + 22), " ");
					strcpy((str + 23), *tzn);
				}

			}
			else
				sprintf((str + 5), "/%04d %02d:%02d %s",
					  -(tm->tm_year - 1), tm->tm_hour, tm->tm_min, "BC");
			break;

			/* German variant on European style */
		case USE_GERMAN_DATES:
			sprintf(str, "%02d.%02d", tm->tm_mday, tm->tm_mon);
			if (tm->tm_year > 0)
			{
				sprintf((str + 5), ".%04d %02d:%02d:%05.2f",
						tm->tm_year, tm->tm_hour, tm->tm_min, sec);

				if ((*tzn != NULL) && (tm->tm_isdst >= 0))
				{
					strcpy((str + 22), " ");
					strcpy((str + 23), *tzn);
				}

			}
			else
				sprintf((str + 5), ".%04d %02d:%02d %s",
					  -(tm->tm_year - 1), tm->tm_hour, tm->tm_min, "BC");
			break;

			/* backward-compatible with traditional Postgres abstime dates */
		case USE_POSTGRES_DATES:
		default:
			day = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday);
			tm->tm_wday = j2day(day);

			strncpy(str, days[tm->tm_wday], 3);
			strcpy((str + 3), " ");

			if (EuroDates)
				sprintf((str + 4), "%02d %3s", tm->tm_mday, months[tm->tm_mon - 1]);
			else
				sprintf((str + 4), "%3s %02d", months[tm->tm_mon - 1], tm->tm_mday);

			if (tm->tm_year > 0)
			{
				sprintf((str + 10), " %02d:%02d", tm->tm_hour, tm->tm_min);
				if (fsec != 0)
				{
					sprintf((str + 16), ":%05.2f %04d", sec, tm->tm_year);
					if ((*tzn != NULL) && (tm->tm_isdst >= 0))
					{
						strcpy((str + 27), " ");
						strncpy((str + 28), *tzn, MAXTZLEN);
					}
				}
				else
				{
					sprintf((str + 16), ":%02.0f %04d", sec, tm->tm_year);
					if ((*tzn != NULL) && (tm->tm_isdst >= 0))
					{
						strcpy((str + 24), " ");
						strncpy((str + 25), *tzn, MAXTZLEN);
					}
				}

			}
			else
			{
				sprintf((str + 10), " %02d:%02d %04d %s",
					  tm->tm_hour, tm->tm_min, -(tm->tm_year - 1), "BC");
			}
			break;
	}

	return TRUE;
}	/* EncodeDateTime() */


/* EncodeTimeSpan()
 * Interpret time structure as a delta time and convert to string.
 *
 * Support "traditional Postgres" and ISO-8601 styles.
 * Actually, afaik ISO does not address time interval formatting,
 *	but this looks similar to the spec for absolute date/time.
 * - thomas 1998-04-30
 */
int
EncodeTimeSpan(struct tm * tm, double fsec, int style, char *str)
{
	int			is_before = FALSE;
	int			is_nonzero = FALSE;
	char	   *cp = str;

	switch (style)
	{
			/* compatible with ISO date formats */
		case USE_ISO_DATES:
			break;

		default:
			strcpy(cp, "@ ");
			cp += strlen(cp);
			break;
	}

	if (tm->tm_year != 0)
	{
		is_before |= (tm->tm_year < 0);
		sprintf(cp, "%d year%s",
				abs(tm->tm_year), ((abs(tm->tm_year) != 1) ? "s" : ""));
		cp += strlen(cp);
		is_nonzero = TRUE;
	}

	if (tm->tm_mon != 0)
	{
		is_before |= (tm->tm_mon < 0);
		sprintf(cp, "%s%d mon%s", (is_nonzero ? " " : ""),
				abs(tm->tm_mon), ((abs(tm->tm_mon) != 1) ? "s" : ""));
		cp += strlen(cp);
		is_nonzero = TRUE;
	}

	switch (style)
	{
			/* compatible with ISO date formats */
		case USE_ISO_DATES:
			if (tm->tm_mday != 0)
			{
				is_before |= (tm->tm_mday < 0);
				sprintf(cp, "%s%d", (is_nonzero ? " " : ""), abs(tm->tm_mday));
				cp += strlen(cp);
				is_nonzero = TRUE;
			}
			is_before |= ((tm->tm_hour < 0) || (tm->tm_min < 0));
			sprintf(cp, "%s%02d:%02d", (is_nonzero ? " " : ""),
					abs(tm->tm_hour), abs(tm->tm_min));
			cp += strlen(cp);
			if ((tm->tm_hour != 0) || (tm->tm_min != 0))
				is_nonzero = TRUE;

			/* fractional seconds? */
			if (fsec != 0)
			{
				fsec += tm->tm_sec;
				is_before |= (fsec < 0);
				sprintf(cp, ":%05.2f", fabs(fsec));
				cp += strlen(cp);
				is_nonzero = TRUE;

				/* otherwise, integer seconds only? */
			}
			else if (tm->tm_sec != 0)
			{
				is_before |= (tm->tm_sec < 0);
				sprintf(cp, ":%02d", abs(tm->tm_sec));
				cp += strlen(cp);
				is_nonzero = TRUE;
			}
			break;

		case USE_POSTGRES_DATES:
		default:
			if (tm->tm_mday != 0)
			{
				is_before |= (tm->tm_mday < 0);
				sprintf(cp, "%s%d day%s", (is_nonzero ? " " : ""),
				 abs(tm->tm_mday), ((abs(tm->tm_mday) != 1) ? "s" : ""));
				cp += strlen(cp);
				is_nonzero = TRUE;
			}
			if (tm->tm_hour != 0)
			{
				is_before |= (tm->tm_hour < 0);
				sprintf(cp, "%s%d hour%s", (is_nonzero ? " " : ""),
				 abs(tm->tm_hour), ((abs(tm->tm_hour) != 1) ? "s" : ""));
				cp += strlen(cp);
				is_nonzero = TRUE;
			}

			if (tm->tm_min != 0)
			{
				is_before |= (tm->tm_min < 0);
				sprintf(cp, "%s%d min%s", (is_nonzero ? " " : ""),
				   abs(tm->tm_min), ((abs(tm->tm_min) != 1) ? "s" : ""));
				cp += strlen(cp);
				is_nonzero = TRUE;
			}

			/* fractional seconds? */
			if (fsec != 0)
			{
				fsec += tm->tm_sec;
				is_before |= (fsec < 0);
				sprintf(cp, "%s%.2f secs", (is_nonzero ? " " : ""), fabs(fsec));
				cp += strlen(cp);
				is_nonzero = TRUE;

				/* otherwise, integer seconds only? */
			}
			else if (tm->tm_sec != 0)
			{
				is_before |= (tm->tm_sec < 0);
				sprintf(cp, "%s%d sec%s", (is_nonzero ? " " : ""),
				   abs(tm->tm_sec), ((abs(tm->tm_sec) != 1) ? "s" : ""));
				cp += strlen(cp);
				is_nonzero = TRUE;
			}
			break;
	}

	/* identically zero? then put in a unitless zero... */
	if (!is_nonzero)
	{
		strcat(cp, "0");
		cp += strlen(cp);
	}

	if (is_before)
	{
		strcat(cp, " ago");
		cp += strlen(cp);
	}

	return 0;
}	/* EncodeTimeSpan() */


#if defined(linux) && defined(__powerpc__)

int
timestamp_is_epoch(double j)
{
	static union
	{
		double		epoch;
		unsigned char c[8];
	}			u;

	u.c[0] = 0x80;				/* sign bit */
	u.c[1] = 0x10;				/* DBL_MIN */

	return j == u.epoch;
}
int
timestamp_is_current(double j)
{
	static union
	{
		double		current;
		unsigned char c[8];
	}			u;

	u.c[1] = 0x10;				/* DBL_MIN */

	return j == u.current;
}

#endif
