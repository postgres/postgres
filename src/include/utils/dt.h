/*-------------------------------------------------------------------------
 *
 * dt.h--
 *	  Definitions for the date/time and other date/time support code.
 *	  The support code is shared with other date data types,
 *	   including abstime, reltime, date, and time.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dt.h,v 1.32 1998/09/01 04:38:59 momjian Exp $
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
 *	separately since the elapsed time spanned is unknown until instantiated
 *	relative to an absolute time.
 *
 * Note that Postgres uses "time interval" to mean a bounded interval,
 *	consisting of a beginning and ending time, not a time span - thomas 97/03/20
 */

typedef double DateTime;

typedef struct
{
	double		time;			/* all time units other than months and
								 * years */
	int4		month;			/* months and years, after time for
								 * alignment */
} TimeSpan;


/* ----------------------------------------------------------------
 *				time types + support macros
 *
 * String definitions for standard time quantities.
 *
 * These strings are the defaults used to form output time strings.
 * Other alternate forms are hardcoded into token tables in dt.c.
 * ----------------------------------------------------------------
 */

#define DAGO			"ago"
#define DCURRENT		"current"
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
#define DMILLENIUM		"millenium"
#define DA_D			"ad"
#define DB_C			"bc"
#define DTIMEZONE		"timezone"

/*
 * Fundamental time field definitions for parsing.
 *
 *	Meridian:  am, pm, or 24-hour style.
 *	Millenium: ad, bc
 */

#define AM		0
#define PM		1
#define HR24	2

#define AD		0
#define BC		1

/*
 * Fields for time decoding.
 * Can't have more of these than there are bits in an unsigned int
 *	since these are turned into bit masks during parsing and decoding.
 */

#define RESERV	0
#define MONTH	1
#define YEAR	2
#define DAY		3
#define TIMES	4				/* not used - thomas 1997-07-14 */
#define TZ		5
#define DTZ		6
#define DTZMOD	7
#define IGNORE	8
#define AMPM	9
#define HOUR	10
#define MINUTE	11
#define SECOND	12
#define DOY		13
#define DOW		14
#define UNITS	15
#define ADBC	16
/* these are only for relative dates */
#define AGO		17
#define ABS_BEFORE		18
#define ABS_AFTER		19

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
#define DTK_MILLENIUM	28
#define DTK_MILLISEC	29
#define DTK_MICROSEC	30

#define DTK_DOW			32
#define DTK_DOY			33
#define DTK_TZ_HOUR		34
#define DTK_TZ_MINUTE	35

/*
 * Bit mask definitions for time parsing.
 */

#define DTK_M(t)		(0x01 << (t))

#define DTK_DATE_M		(DTK_M(YEAR) | DTK_M(MONTH) | DTK_M(DAY))
#define DTK_TIME_M		(DTK_M(HOUR) | DTK_M(MINUTE) | DTK_M(SECOND))

#define MAXDATELEN		47		/* maximum possible length of an input
								 * date string */
#define MAXDATEFIELDS	25		/* maximum possible number of fields in a
								 * date string */
#define TOKMAXLEN		10		/* only this many chars are stored in
								 * datetktbl */

/* keep this struct small; it gets used a lot */
typedef struct
{
#if defined(_AIX)
	char	   *token;
#else
	char		token[TOKMAXLEN];
#endif	 /* _AIX */
	char		type;
	char		value;			/* this may be unsigned, alas */
} datetkn;

#ifdef NAN
#define DT_INVALID		(NAN)
#else
#define DT_INVALID		(DBL_MIN+DBL_MIN)
#endif
#ifdef HUGE_VAL
#define DT_NOBEGIN		(-HUGE_VAL)
#define DT_NOEND		(HUGE_VAL)
#else
#define DT_NOBEGIN		(-DBL_MAX)
#define DT_NOEND		(DBL_MAX)
#endif
#define DT_CURRENT		(DBL_MIN)
#define DT_EPOCH		(-DBL_MIN)

#define DATETIME_INVALID(j)		{j = DT_INVALID;}
#ifdef NAN
#define DATETIME_IS_INVALID(j)	(isnan(j))
#else
#define DATETIME_IS_INVALID(j)	(j == DT_INVALID)
#endif

#define DATETIME_NOBEGIN(j)		{j = DT_NOBEGIN;}
#define DATETIME_IS_NOBEGIN(j)	(j == DT_NOBEGIN)

#define DATETIME_NOEND(j)		{j = DT_NOEND;}
#define DATETIME_IS_NOEND(j)	(j == DT_NOEND)

#define DATETIME_CURRENT(j)		{j = DT_CURRENT;}
#if defined(linux) && defined(PPC)
extern int	datetime_is_current(double j);

#define DATETIME_IS_CURRENT(j)	datetime_is_current(j)
#else
#define DATETIME_IS_CURRENT(j)	(j == DT_CURRENT)
#endif

#define DATETIME_EPOCH(j)		{j = DT_EPOCH;}
#if defined(linux) && defined(PPC)
extern int	datetime_is_epoch(double j);

#define DATETIME_IS_EPOCH(j)	datetime_is_epoch(j)
#else
#define DATETIME_IS_EPOCH(j)	(j == DT_EPOCH)
#endif

#define DATETIME_IS_RELATIVE(j) (DATETIME_IS_CURRENT(j) || DATETIME_IS_EPOCH(j))
#define DATETIME_NOT_FINITE(j)	(DATETIME_IS_INVALID(j) \
								|| DATETIME_IS_NOBEGIN(j) || DATETIME_IS_NOEND(j))
#define DATETIME_IS_RESERVED(j) (DATETIME_IS_RELATIVE(j) || DATETIME_NOT_FINITE(j))

#define TIMESPAN_INVALID(j)		{(j).time = DT_INVALID;}
#ifdef NAN
#define TIMESPAN_IS_INVALID(j)	(isnan((j).time))
#else
#define TIMESPAN_IS_INVALID(j)	((j).time == DT_INVALID)
#endif
#define TIMESPAN_NOT_FINITE(j)	TIMESPAN_IS_INVALID(j)

#define TIME_PREC_INV 1000000.0
#define JROUND(j) (rint(((double) (j))*TIME_PREC_INV)/TIME_PREC_INV)

/*
 * dt.c prototypes
 */

extern DateTime *datetime_in(char *str);
extern char *datetime_out(DateTime *dt);
extern bool datetime_eq(DateTime *dt1, DateTime *dt2);
extern bool datetime_ne(DateTime *dt1, DateTime *dt2);
extern bool datetime_lt(DateTime *dt1, DateTime *dt2);
extern bool datetime_le(DateTime *dt1, DateTime *dt2);
extern bool datetime_ge(DateTime *dt1, DateTime *dt2);
extern bool datetime_gt(DateTime *dt1, DateTime *dt2);
extern bool datetime_finite(DateTime *datetime);
extern int	datetime_cmp(DateTime *dt1, DateTime *dt2);
extern DateTime *datetime_smaller(DateTime *dt1, DateTime *dt2);
extern DateTime *datetime_larger(DateTime *dt1, DateTime *dt2);

extern TimeSpan *timespan_in(char *str);
extern char *timespan_out(TimeSpan *span);
extern bool timespan_eq(TimeSpan *span1, TimeSpan *span2);
extern bool timespan_ne(TimeSpan *span1, TimeSpan *span2);
extern bool timespan_lt(TimeSpan *span1, TimeSpan *span2);
extern bool timespan_le(TimeSpan *span1, TimeSpan *span2);
extern bool timespan_ge(TimeSpan *span1, TimeSpan *span2);
extern bool timespan_gt(TimeSpan *span1, TimeSpan *span2);
extern bool timespan_finite(TimeSpan *span);
extern int	timespan_cmp(TimeSpan *span1, TimeSpan *span2);
extern TimeSpan *timespan_smaller(TimeSpan *span1, TimeSpan *span2);
extern TimeSpan *timespan_larger(TimeSpan *span1, TimeSpan *span2);

extern text *datetime_text(DateTime *datetime);
extern DateTime *text_datetime(text *str);
extern text *timespan_text(TimeSpan *timespan);
extern TimeSpan *text_timespan(text *str);
extern DateTime *datetime_trunc(text *units, DateTime *datetime);
extern TimeSpan *timespan_trunc(text *units, TimeSpan *timespan);
extern float64 datetime_part(text *units, DateTime *datetime);
extern float64 timespan_part(text *units, TimeSpan *timespan);
extern text *datetime_zone(text *zone, DateTime *datetime);

extern TimeSpan *timespan_um(TimeSpan *span);
extern TimeSpan *timespan_pl(TimeSpan *span1, TimeSpan *span2);
extern TimeSpan *timespan_mi(TimeSpan *span1, TimeSpan *span2);
extern TimeSpan *timespan_div(TimeSpan *span1, float8 *arg2);

extern TimeSpan *datetime_mi(DateTime *dt1, DateTime *dt2);
extern DateTime *datetime_pl_span(DateTime *dt, TimeSpan *span);
extern DateTime *datetime_mi_span(DateTime *dt, TimeSpan *span);
extern TimeSpan *datetime_age(DateTime *dt1, DateTime *dt2);

extern void GetCurrentTime(struct tm * tm);
extern DateTime SetDateTime(DateTime datetime);
extern int	tm2datetime(struct tm * tm, double fsec, int *tzp, DateTime *dt);
extern int	datetime2tm(DateTime dt, int *tzp, struct tm * tm, double *fsec, char **tzn);
extern int	timespan2tm(TimeSpan span, struct tm * tm, float8 *fsec);
extern int	tm2timespan(struct tm * tm, double fsec, TimeSpan *span);

extern void j2date(int jd, int *year, int *month, int *day);
extern int	date2j(int year, int month, int day);

extern double time2t(const int hour, const int min, const double sec);

extern int ParseDateTime(char *timestr, char *lowstr,
			  char **field, int *ftype, int maxfields, int *numfields);
extern int DecodeDateTime(char **field, int *ftype,
			 int nf, int *dtype, struct tm * tm, double *fsec, int *tzp);

extern int DecodeTimeOnly(char **field, int *ftype, int nf,
			   int *dtype, struct tm * tm, double *fsec);

extern int DecodeDateDelta(char **field, int *ftype,
				int nf, int *dtype, struct tm * tm, double *fsec);

extern int	EncodeDateOnly(struct tm * tm, int style, char *str);
extern int	EncodeTimeOnly(struct tm * tm, double fsec, int style, char *str);
extern int	EncodeDateTime(struct tm * tm, double fsec, int *tzp, char **tzn, int style, char *str);
extern int	EncodeTimeSpan(struct tm * tm, double fsec, int style, char *str);

#endif	 /* DT_H */
