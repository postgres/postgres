/*-------------------------------------------------------------------------
 *
 * dt.h--
 *    Definitions for the date/time and other date/time support code.
 *    The support code is shared with other date data types,
 *     including abstime, reltime, date, and time.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dt.h,v 1.5 1997/04/02 18:32:20 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DT_H
#define DT_H

#include <time.h>
#include <math.h>

/*
 * DateTime represents absolute time.
 * TimeSpan represents delta time. Keep track of months (and years)
 *  separately since the elapsed time spanned is unknown until instantiated
 *  relative to an absolute time.
 *
 * Note that Postgres uses "time interval" to mean a bounded interval,
 *  consisting of a beginning and ending time, not a time span - tgl 97/03/20
 */

typedef double DateTime;

typedef struct {
    double      time;   /* all time units other than months and years */
    int4        month;  /* months and years, after time for alignment */
} TimeSpan;


/*
 * USE_NEW_DATE enables a more efficient Julian day-based date type.
 * USE_NEW_TIME enables a more efficient double-based time type.
 * These have been tested in v6.1beta, but only by myself.
 * These should be enabled for Postgres v7.0 - tgl 97/04/02
 */
#define USE_NEW_DATE		0
#define USE_NEW_TIME		0


/* ----------------------------------------------------------------
 *		time types + support macros
 *
 * String definitions for standard time quantities.
 *
 * These strings are the defaults used to form output time strings.
 * Other alternate forms are hardcoded into token tables in dt.c.
 * ----------------------------------------------------------------
 */

#define DAGO		"ago"
#define DCURRENT	"current"
#define EPOCH		"epoch"
#define INVALID		"invalid"
#define EARLY		"-infinity"
#define LATE		"infinity"
#define NOW		"now"
#define TODAY		"today"
#define TOMORROW	"tomorrow"
#define YESTERDAY	"yesterday"
#define ZULU		"zulu"

#define DMICROSEC	"usecond"
#define DMILLISEC	"msecond"
#define DSECOND		"second"
#define DMINUTE		"minute"
#define DHOUR		"hour"
#define DDAY		"day"
#define DWEEK		"week"
#define DMONTH		"month"
#define DQUARTER	"quarter"
#define DYEAR		"year"
#define DDECADE		"decade"
#define DCENTURY	"century"
#define DMILLENIUM	"millenium"
#define DA_D		"ad"
#define DB_C		"bc"
#define DTIMEZONE	"timezone"

/*
 * Fundamental time field definitions for parsing.
 *
 *  Meridian:  am, pm, or 24-hour style.
 *  Millenium: ad, bc
 */

#define AM	0
#define PM	1
#define HR24	2

#define AD	0
#define BC	1

/*
 * Fields for time decoding.
 * Can't have more of these than there are bits in an unsigned int
 *  since these are turned into bit masks during parsing and decoding.
 */

#define RESERV	0
#define MONTH	1
#define YEAR	2
#define DAY	3
#define TIME	4
#define TZ	5
#define DTZ	6
#define IGNORE	7
#define AMPM	8
#define HOUR	9
#define MINUTE	10
#define SECOND	11
#define DOY	12
#define DOW	13
#define UNITS	14
#define ADBC	15
/* these are only for relative dates */
#define ABS_BEFORE	14
#define ABS_AFTER	15
#define AGO	16

/*
 * Token field definitions for time parsing and decoding.
 * These need to fit into the datetkn table type.
 * At the moment, that means keep them within [-127,127].
 */

#define DTK_NUMBER	0
#define DTK_STRING	1

#define DTK_DATETIME	2
#define DTK_DATE	3
#define DTK_TIME	4
#define DTK_TZ		5

#define DTK_SPECIAL	6
#define DTK_INVALID	7
#define DTK_CURRENT	8
#define DTK_EARLY	9
#define DTK_LATE	10
#define DTK_EPOCH	11
#define DTK_NOW		12
#define DTK_YESTERDAY	13
#define DTK_TODAY	14
#define DTK_TOMORROW	15
#define DTK_ZULU	16

#define DTK_DELTA	32
#define DTK_SECOND	33
#define DTK_MINUTE	34
#define DTK_HOUR	35
#define DTK_DAY		36
#define DTK_WEEK	37
#define DTK_MONTH	38
#define DTK_QUARTER	39
#define DTK_YEAR	40
#define DTK_DECADE	41
#define DTK_CENTURY	42
#define DTK_MILLENIUM	43
#define DTK_MILLISEC	44
#define DTK_MICROSEC	45
#define DTK_AGO		46

/*
 * Bit mask definitions for time parsing.
 */

#define DTK_M(t)	(0x01 << t)

#define DTK_DATE_M	(DTK_M(YEAR) | DTK_M(MONTH) | DTK_M(DAY))
#define DTK_TIME_M	(DTK_M(HOUR) | DTK_M(MINUTE) | DTK_M(SECOND))

#define MAXDATELEN	47	/* maximum possible length of an input date string */
#define MAXDATEFIELDS	25	/* maximum possible number of fields in a date string */
#define TOKMAXLEN	10	/* only this many chars are stored in datetktbl */

/* keep this struct small; it gets used a lot */
typedef struct {
#if defined(aix)
    char *token;
#else
    char token[TOKMAXLEN];
#endif /* aix */
    char type;
    char value;		/* this may be unsigned, alas */
} datetkn;

#ifdef NAN
#define DT_INVALID	(NAN)
#else
#define DT_INVALID	(DBL_MIN+DBL_MIN)
#endif
#ifdef HUGE_VAL
#define DT_NOBEGIN	(-HUGE_VAL)
#define DT_NOEND	(HUGE_VAL)
#else
#define DT_NOBEGIN	(-DBL_MAX)
#define DT_NOEND	(DBL_MAX)
#endif
#define DT_CURRENT	(DBL_MIN)
#define DT_EPOCH	(-DBL_MIN)

#define DATETIME_INVALID(j)	{j = DT_INVALID;}
#ifdef NAN
#define DATETIME_IS_INVALID(j)	(isnan(j))
#else
#define DATETIME_IS_INVALID(j)	(j == DT_INVALID)
#endif

#define DATETIME_NOBEGIN(j)	{j = DT_NOBEGIN;}
#define DATETIME_IS_NOBEGIN(j)	(j == DT_NOBEGIN)

#define DATETIME_NOEND(j)	{j = DT_NOEND;}
#define DATETIME_IS_NOEND(j)	(j == DT_NOEND)

#define DATETIME_CURRENT(j)	{j = DT_CURRENT;}
#define DATETIME_IS_CURRENT(j)	(j == DT_CURRENT)

#define DATETIME_EPOCH(j)	{j = DT_EPOCH;}
#define DATETIME_IS_EPOCH(j)	(j == DT_EPOCH)

#define DATETIME_IS_RELATIVE(j)	(DATETIME_IS_CURRENT(j) || DATETIME_IS_EPOCH(j))
#define DATETIME_NOT_FINITE(j)	(DATETIME_IS_INVALID(j) \
				|| DATETIME_IS_NOBEGIN(j) || DATETIME_IS_NOEND(j))
#define DATETIME_IS_RESERVED(j) (DATETIME_IS_RELATIVE(j) || DATETIME_NOT_FINITE(j))

#define TIMESPAN_INVALID(j)	{(j).time = DT_INVALID;}
#ifdef NAN
#define TIMESPAN_IS_INVALID(j)	(isnan((j).time))
#else
#define TIMESPAN_IS_INVALID(j)	((j).time == DT_INVALID)
#endif
#define TIMESPAN_NOT_FINITE(j)	TIMESPAN_IS_INVALID(j)

#define TIME_PREC 1e-6
#define JROUND(j) (rint(((double) j)/TIME_PREC)*TIME_PREC)

/*
 * dt.c prototypes 
 */

extern DateTime *datetime_in( char *str);
extern char *datetime_out( DateTime *dt);
extern bool datetime_eq(DateTime *dt1, DateTime *dt2);
extern bool datetime_ne(DateTime *dt1, DateTime *dt2);
extern bool datetime_lt(DateTime *dt1, DateTime *dt2);
extern bool datetime_le(DateTime *dt1, DateTime *dt2);
extern bool datetime_ge(DateTime *dt1, DateTime *dt2);
extern bool datetime_gt(DateTime *dt1, DateTime *dt2);
extern bool datetime_finite(DateTime *datetime);

extern TimeSpan *timespan_in(char *str);
extern char *timespan_out(TimeSpan *span);
extern bool timespan_eq(TimeSpan *span1, TimeSpan *span2);
extern bool timespan_ne(TimeSpan *span1, TimeSpan *span2);
extern bool timespan_lt(TimeSpan *span1, TimeSpan *span2);
extern bool timespan_le(TimeSpan *span1, TimeSpan *span2);
extern bool timespan_ge(TimeSpan *span1, TimeSpan *span2);
extern bool timespan_gt(TimeSpan *span1, TimeSpan *span2);
extern bool timespan_finite(TimeSpan *span);

extern text *datetime_text(DateTime *datetime);
extern DateTime *text_datetime(text *str);
extern text *timespan_text(TimeSpan *timespan);
extern TimeSpan *text_timespan(text *str);
extern float64 datetime_part(text *units, DateTime *datetime);
extern float64 timespan_part(text *units, TimeSpan *timespan);

extern TimeSpan *timespan_um(TimeSpan *span);
extern TimeSpan *timespan_add(TimeSpan *span1, TimeSpan *span2);
extern TimeSpan *timespan_sub(TimeSpan *span1, TimeSpan *span2);

extern TimeSpan *datetime_sub(DateTime *dt1, DateTime *dt2);
extern DateTime *datetime_add_span(DateTime *dt, TimeSpan *span);
extern DateTime *datetime_sub_span(DateTime *dt, TimeSpan *span);

extern void GetCurrentTime(struct tm *tm);
extern DateTime SetDateTime(DateTime datetime);
extern DateTime tm2datetime(struct tm *tm, double fsec, int *tzp);
extern int datetime2tm( DateTime dt, int *tzp, struct tm *tm, double *fsec);

extern int timespan2tm(TimeSpan span, struct tm *tm, float8 *fsec);
extern int tm2timespan(struct tm *tm, double fsec, TimeSpan *span);

extern DateTime dt2local( DateTime dt, int timezone);

extern void j2date( int jd, int *year, int *month, int *day);
extern int date2j( int year, int month, int day);
extern int j2day( int jd);

extern double time2t(const int hour, const int min, const double sec);
extern void dt2time(DateTime dt, int *hour, int *min, double *sec);

extern int ParseDateTime( char *timestr, char *lowstr,
  char *field[], int ftype[], int maxfields, int *numfields);
extern int DecodeDateTime( char *field[], int ftype[],
 int nf, int *dtype, struct tm *tm, double *fsec, int *tzp);
extern int DecodeDate(char *str, int fmask, int *tmask, struct tm *tm);
extern int DecodeNumber( int flen, char *field,
 int fmask, int *tmask, struct tm *tm, double *fsec);
extern int DecodeNumberField( int len, char *str,
 int fmask, int *tmask, struct tm *tm, double *fsec);
extern int DecodeTime(char *str,
 int fmask, int *tmask, struct tm *tm, double *fsec);
extern int DecodeTimeOnly( char *field[], int ftype[], int nf,
 int *dtype, struct tm *tm, double *fsec);
extern int DecodeTimezone( char *str, int *tzp);
extern int DecodeSpecial(int field, char *lowtoken, int *val);

extern int DecodeDateDelta( char *field[], int ftype[],
 int nf, int *dtype, struct tm *tm, double *fsec);
extern int DecodeUnits(int field, char *lowtoken, int *val);

extern int EncodeSpecialDateTime(DateTime dt, char *str);
extern int EncodeDateTime(struct tm *tm, double fsec, int style, char *str);
extern int EncodeTimeSpan(struct tm *tm, double fsec, int style, char *str);

extern datetkn *datebsearch(char *key, datetkn *base, unsigned int nel);

#endif /* DT_H */
