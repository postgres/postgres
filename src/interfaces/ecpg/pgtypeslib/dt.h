/* src/interfaces/ecpg/pgtypeslib/dt.h */

#ifndef DT_H
#define DT_H

#include <pgtypes_timestamp.h>

#define MAXTZLEN			 10

#ifdef HAVE_INT64_TIMESTAMP

typedef int32 fsec_t;
#else

typedef double fsec_t;

/* round off to MAX_TIMESTAMP_PRECISION decimal places */
/* note: this is also used for rounding off intervals */
#define TS_PREC_INV 1000000.0
#define TSROUND(j) (rint(((double) (j)) * TS_PREC_INV) / TS_PREC_INV)
#endif

#define USE_POSTGRES_DATES				0
#define USE_ISO_DATES					1
#define USE_SQL_DATES					2
#define USE_GERMAN_DATES				3

#define INTSTYLE_POSTGRES			  0
#define INTSTYLE_POSTGRES_VERBOSE	  1
#define INTSTYLE_SQL_STANDARD		  2
#define INTSTYLE_ISO_8601			  3

#define INTERVAL_FULL_RANGE (0x7FFF)
#define INTERVAL_MASK(b) (1 << (b))
#define MAX_INTERVAL_PRECISION 6

#define DTERR_BAD_FORMAT		(-1)
#define DTERR_FIELD_OVERFLOW	(-2)
#define DTERR_MD_FIELD_OVERFLOW (-3)	/* triggers hint about DateStyle */
#define DTERR_INTERVAL_OVERFLOW (-4)
#define DTERR_TZDISP_OVERFLOW	(-5)


#define DAGO			"ago"
#define EPOCH			"epoch"
#define INVALID			"invalid"
#define EARLY			"-infinity"
#define LATE			"infinity"
#define NOW				"now"
#define TODAY			"today"
#define TOMORROW		"tomorrow"
#define YESTERDAY		"yesterday"
#define ZULU			"zulu"

#define DMICROSEC		"usecond"
#define DMILLISEC		"msecond"
#define DSECOND			"second"
#define DMINUTE			"minute"
#define DHOUR			"hour"
#define DDAY			"day"
#define DWEEK			"week"
#define DMONTH			"month"
#define DQUARTER		"quarter"
#define DYEAR			"year"
#define DDECADE			"decade"
#define DCENTURY		"century"
#define DMILLENNIUM		"millennium"
#define DA_D			"ad"
#define DB_C			"bc"
#define DTIMEZONE		"timezone"
#define DCURRENT		   "current"

/*
 * Fundamental time field definitions for parsing.
 *
 *	Meridian:  am, pm, or 24-hour style.
 *	Millennium: ad, bc
 */

#define AM		0
#define PM		1
#define HR24	2

#define AD		0
#define BC		1

/*
 * Fields for time decoding.
 *
 * Can't have more of these than there are bits in an unsigned int
 * since these are turned into bit masks during parsing and decoding.
 *
 * Furthermore, the values for YEAR, MONTH, DAY, HOUR, MINUTE, SECOND
 * must be in the range 0..14 so that the associated bitmasks can fit
 * into the left half of an INTERVAL's typmod value.
 *
 * Copy&pasted these values from src/include/utils/datetime.h
 * 2008-11-20, changing a number of their values.
 */

#define RESERV	0
#define MONTH	1
#define YEAR	2
#define DAY		3
#define JULIAN	4
#define TZ		5
#define DTZ		6
#define DTZMOD	7
#define IGNORE_DTF	8
#define AMPM	9
#define HOUR	10
#define MINUTE	11
#define SECOND	12
#define MILLISECOND 13
#define MICROSECOND 14
#define DOY		15
#define DOW		16
#define UNITS	17
#define ADBC	18
/* these are only for relative dates */
#define AGO		19
#define ABS_BEFORE		20
#define ABS_AFTER		21
/* generic fields to help with parsing */
#define ISODATE 22
#define ISOTIME 23
/* reserved for unrecognized string values */
#define UNKNOWN_FIELD	31


/*
 * Token field definitions for time parsing and decoding.
 * These need to fit into the datetkn table type.
 * At the moment, that means keep them within [-127,127].
 * These are also used for bit masks in DecodeDateDelta()
 *	so actually restrict them to within [0,31] for now.
 * - thomas 97/06/19
 * Not all of these fields are used for masks in DecodeDateDelta
 *	so allow some larger than 31. - thomas 1997-11-17
 */

#define DTK_NUMBER		0
#define DTK_STRING		1

#define DTK_DATE		2
#define DTK_TIME		3
#define DTK_TZ			4
#define DTK_AGO			5

#define DTK_SPECIAL		6
#define DTK_INVALID		7
#define DTK_CURRENT		8
#define DTK_EARLY		9
#define DTK_LATE		10
#define DTK_EPOCH		11
#define DTK_NOW			12
#define DTK_YESTERDAY	13
#define DTK_TODAY		14
#define DTK_TOMORROW	15
#define DTK_ZULU		16

#define DTK_DELTA		17
#define DTK_SECOND		18
#define DTK_MINUTE		19
#define DTK_HOUR		20
#define DTK_DAY			21
#define DTK_WEEK		22
#define DTK_MONTH		23
#define DTK_QUARTER		24
#define DTK_YEAR		25
#define DTK_DECADE		26
#define DTK_CENTURY		27
#define DTK_MILLENNIUM	28
#define DTK_MILLISEC	29
#define DTK_MICROSEC	30
#define DTK_JULIAN		31

#define DTK_DOW			32
#define DTK_DOY			33
#define DTK_TZ_HOUR		34
#define DTK_TZ_MINUTE	35
#define DTK_ISOYEAR		36
#define DTK_ISODOW		37


/*
 * Bit mask definitions for time parsing.
 */
/* Copy&pasted these values from src/include/utils/datetime.h */
#define DTK_M(t)		(0x01 << (t))
#define DTK_ALL_SECS_M	   (DTK_M(SECOND) | DTK_M(MILLISECOND) | DTK_M(MICROSECOND))
#define DTK_DATE_M		(DTK_M(YEAR) | DTK_M(MONTH) | DTK_M(DAY))
#define DTK_TIME_M		(DTK_M(HOUR) | DTK_M(MINUTE) | DTK_M(SECOND))

#define MAXDATELEN		63		/* maximum possible length of an input date
								 * string (not counting tr. null) */
#define MAXDATEFIELDS	25		/* maximum possible number of fields in a date
								 * string */
#define TOKMAXLEN		10		/* only this many chars are stored in
								 * datetktbl */

/* keep this struct small; it gets used a lot */
typedef struct
{
#if defined(_AIX)
	char	   *token;
#else
	char		token[TOKMAXLEN];
#endif   /* _AIX */
	char		type;
	char		value;			/* this may be unsigned, alas */
} datetkn;


/* FMODULO()
 * Macro to replace modf(), which is broken on some platforms.
 * t = input and remainder
 * q = integer part
 * u = divisor
 */
#define FMODULO(t,q,u) \
do { \
	(q) = (((t) < 0) ? ceil((t) / (u)): floor((t) / (u))); \
	if ((q) != 0) (t) -= rint((q) * (u)); \
} while(0)

/* TMODULO()
 * Like FMODULO(), but work on the timestamp datatype (either int64 or float8).
 * We assume that int64 follows the C99 semantics for division (negative
 * quotients truncate towards zero).
 */
#ifdef HAVE_INT64_TIMESTAMP
#define TMODULO(t,q,u) \
do { \
	(q) = ((t) / (u)); \
	if ((q) != 0) (t) -= ((q) * (u)); \
} while(0)
#else
#define TMODULO(t,q,u) \
do { \
	(q) = (((t) < 0) ? ceil((t) / (u)): floor((t) / (u))); \
	if ((q) != 0) (t) -= rint((q) * (u)); \
} while(0)
#endif

/* in both timestamp.h and ecpg/dt.h */
#define DAYS_PER_YEAR	365.25	/* assumes leap year every four years */
#define MONTHS_PER_YEAR 12
/*
 *	DAYS_PER_MONTH is very imprecise.  The more accurate value is
 *	365.2425/12 = 30.436875, or '30 days 10:29:06'.  Right now we only
 *	return an integral number of days, but someday perhaps we should
 *	also return a 'time' value to be used as well.	ISO 8601 suggests
 *	30 days.
 */
#define DAYS_PER_MONTH	30		/* assumes exactly 30 days per month */
#define HOURS_PER_DAY	24		/* assume no daylight savings time changes */

/*
 *	This doesn't adjust for uneven daylight savings time intervals or leap
 *	seconds, and it crudely estimates leap years.  A more accurate value
 *	for days per years is 365.2422.
 */
#define SECS_PER_YEAR	(36525 * 864)	/* avoid floating-point computation */
#define SECS_PER_DAY	86400
#define SECS_PER_HOUR	3600
#define SECS_PER_MINUTE 60
#define MINS_PER_HOUR	60

#ifdef HAVE_INT64_TIMESTAMP
#define USECS_PER_DAY	INT64CONST(86400000000)
#define USECS_PER_HOUR	INT64CONST(3600000000)
#define USECS_PER_MINUTE INT64CONST(60000000)
#define USECS_PER_SEC	INT64CONST(1000000)
#endif

/*
 * Date/time validation
 * Include check for leap year.
 */
#define isleap(y) (((y) % 4) == 0 && (((y) % 100) != 0 || ((y) % 400) == 0))

/* Julian date support for date2j() and j2date()
 *
 * IS_VALID_JULIAN checks the minimum date exactly, but is a bit sloppy
 * about the maximum, since it's far enough out to not be especially
 * interesting.
 */

#define JULIAN_MINYEAR (-4713)
#define JULIAN_MINMONTH (11)
#define JULIAN_MINDAY (24)
#define JULIAN_MAXYEAR (5874898)

#define IS_VALID_JULIAN(y,m,d) ((((y) > JULIAN_MINYEAR) \
  || (((y) == JULIAN_MINYEAR) && (((m) > JULIAN_MINMONTH) \
  || (((m) == JULIAN_MINMONTH) && ((d) >= JULIAN_MINDAY))))) \
 && ((y) < JULIAN_MAXYEAR))

#define UTIME_MINYEAR (1901)
#define UTIME_MINMONTH (12)
#define UTIME_MINDAY (14)
#define UTIME_MAXYEAR (2038)
#define UTIME_MAXMONTH (01)
#define UTIME_MAXDAY (18)

#define IS_VALID_UTIME(y,m,d) ((((y) > UTIME_MINYEAR) \
 || (((y) == UTIME_MINYEAR) && (((m) > UTIME_MINMONTH) \
  || (((m) == UTIME_MINMONTH) && ((d) >= UTIME_MINDAY))))) \
 && (((y) < UTIME_MAXYEAR) \
 || (((y) == UTIME_MAXYEAR) && (((m) < UTIME_MAXMONTH) \
  || (((m) == UTIME_MAXMONTH) && ((d) <= UTIME_MAXDAY))))))

#ifdef HAVE_INT64_TIMESTAMP

#define DT_NOBEGIN		(-INT64CONST(0x7fffffffffffffff) - 1)
#define DT_NOEND		(INT64CONST(0x7fffffffffffffff))
#else

#ifdef HUGE_VAL
#define DT_NOBEGIN		(-HUGE_VAL)
#define DT_NOEND		(HUGE_VAL)
#else
#define DT_NOBEGIN		(-DBL_MAX)
#define DT_NOEND		(DBL_MAX)
#endif
#endif   /* HAVE_INT64_TIMESTAMP */

#define TIMESTAMP_NOBEGIN(j)	do {(j) = DT_NOBEGIN;} while (0)
#define TIMESTAMP_NOEND(j)			do {(j) = DT_NOEND;} while (0)
#define TIMESTAMP_IS_NOBEGIN(j) ((j) == DT_NOBEGIN)
#define TIMESTAMP_IS_NOEND(j)	((j) == DT_NOEND)
#define TIMESTAMP_NOT_FINITE(j) (TIMESTAMP_IS_NOBEGIN(j) || TIMESTAMP_IS_NOEND(j))

int			DecodeInterval(char **, int *, int, int *, struct tm *, fsec_t *);
int			DecodeTime(char *, int *, struct tm *, fsec_t *);
int			EncodeDateTime(struct tm * tm, fsec_t fsec, bool print_tz, int tz, const char *tzn, int style, char *str, bool EuroDates);
int			EncodeInterval(struct tm * tm, fsec_t fsec, int style, char *str);
int			tm2timestamp(struct tm *, fsec_t, int *, timestamp *);
int			DecodeUnits(int field, char *lowtoken, int *val);
bool		CheckDateTokenTables(void);
int			EncodeDateOnly(struct tm * tm, int style, char *str, bool EuroDates);
int			GetEpochTime(struct tm *);
int			ParseDateTime(char *, char *, char **, int *, int *, char **);
int			DecodeDateTime(char **, int *, int, int *, struct tm *, fsec_t *, bool);
void		j2date(int, int *, int *, int *);
void		GetCurrentDateTime(struct tm *);
int			date2j(int, int, int);
void		TrimTrailingZeros(char *);
void		dt2time(double, int *, int *, int *, fsec_t *);

extern char *pgtypes_date_weekdays_short[];
extern char *pgtypes_date_months[];
extern char *months[];
extern char *days[];
extern int	day_tab[2][13];

#endif   /* DT_H */
