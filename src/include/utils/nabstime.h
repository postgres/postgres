/*-------------------------------------------------------------------------
 *
 * nabstime.h--
 *    Definitions for the "new" abstime code.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nabstime.h,v 1.2 1996/11/01 09:19:11 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NABSTIME_H
#define NABSTIME_H

#include <time.h>

/* ----------------------------------------------------------------
 *		time types + support macros
 *
 *
 * ----------------------------------------------------------------
 */
typedef int32	AbsoluteTime;
typedef int32	RelativeTime;

typedef struct { 
    int32		status;
    AbsoluteTime	data[2];
} TimeIntervalData;
typedef TimeIntervalData *TimeInterval;

#define EPOCH_ABSTIME	((AbsoluteTime) 0)
#define INVALID_ABSTIME ((AbsoluteTime) 2147483647) /* 2^31 - 1 */
#define CURRENT_ABSTIME ((AbsoluteTime) 2147483646) /* 2^31 - 2 */
#define NOEND_ABSTIME	((AbsoluteTime) 2147483645) /* 2^31 - 3 */


#if defined(PORTNAME_aix)
/*
 * AIX considers 2147483648 == -2147483648 (since they have the same bit
 * representation) but uses a different sign sense in a comparison to 
 * these integer constants depending on whether the constant is signed 
 * or not!
 */
#include <values.h>
/*#define NOSTART_ABSTIME	((AbsoluteTime) HIBITI)	*/	/* - 2^31 */
#define NOSTART_ABSTIME      ((AbsoluteTime) INT_MIN)
#else
/*#define NOSTART_ABSTIME ((AbsoluteTime) 2147483648)*/	/* - 2^31 */
#define NOSTART_ABSTIME ((AbsoluteTime) -2147483647)	/* - 2^31 */
#endif /* PORTNAME_aix */

#define INVALID_RELTIME ((RelativeTime) 2147483647)	/* 2^31 - 1 */

/* ----------------
 *	time support macros (from tim.h)
 * ----------------
 */

#define AbsoluteTimeIsValid(time) \
    ((bool) ((time) != INVALID_ABSTIME))

#define AbsoluteTimeIsReal(time) \
    ((bool) (((AbsoluteTime) time) < NOEND_ABSTIME && \
	     ((AbsoluteTime) time) > NOSTART_ABSTIME))

/* have to include this because EPOCH_ABSTIME used to be invalid - yuk */
#define AbsoluteTimeIsBackwardCompatiblyValid(time) \
    ((bool) (((AbsoluteTime) time) != INVALID_ABSTIME && \
	     ((AbsoluteTime) time) > EPOCH_ABSTIME))

#define AbsoluteTimeIsBackwardCompatiblyReal(time) \
    ((bool) (((AbsoluteTime) time) < NOEND_ABSTIME && \
	     ((AbsoluteTime) time) > NOSTART_ABSTIME && \
             ((AbsoluteTime) time) > EPOCH_ABSTIME))

#define RelativeTimeIsValid(time) \
    ((bool) (((RelativeTime) time) != INVALID_RELTIME))

#define GetCurrentAbsoluteTime() \
    ((AbsoluteTime) getSystemTime())

/*
 * getSystemTime --
 *	Returns system time.
 */
#define getSystemTime() \
    ((time_t) (time(0l)))


/*
 *  Meridian:  am, pm, or 24-hour style.
 */
#define AM 0
#define PM 1
#define HR24 2

/* can't have more of these than there are bits in an unsigned long */
#define MONTH	1
#define YEAR	2
#define DAY	3
#define TIME	4
#define TZ	5
#define DTZ	6
#define PG_IGNORE	7
#define AMPM	8
/* below here are unused so far */
#define SECONDS	9
#define MONTHS	10
#define YEARS	11
#define NUMBER	12
/* these are only for relative dates */
#define ABS_BEFORE	13
#define ABS_AFTER	14
#define AGO	15


#define SECS(n)		((time_t)(n))
#define MINS(n)		((time_t)(n) * SECS(60))
#define HOURS(n)	((time_t)(n) * MINS(60))	/* 3600 secs */
#define DAYS(n)		((time_t)(n) * HOURS(24))	/* 86400 secs */
/* months and years are not constant length, must be specially dealt with */

#define TOKMAXLEN 6	/* only this many chars are stored in datetktbl */

/* keep this struct small; it gets used a lot */
typedef struct {
#if defined(PORTNAME_aix)
    char *token;
#else
    char token[TOKMAXLEN];
#endif /* PORTNAME_aix */
    char type;
    char value;		/* this may be unsigned, alas */
} datetkn;

/*
 * nabstime.c prototypes 
 */
extern AbsoluteTime nabstimein(char *timestr);
extern int prsabsdate(char *timestr, struct tm *tm, int *tzp);
extern int tryabsdate(char *fields[], int nf, struct tm *tm, int *tzp);
extern int parsetime(char *time, struct tm *tm);
extern int split(char *string, char *fields[], int nfields, char *sep);
extern char *nabstimeout(AbsoluteTime time);
extern AbsoluteTime dateconv(struct tm *tm, int zone);
extern time_t qmktime(struct tm *tp);
extern datetkn *datetoktype(char *s, int *bigvalp);
extern datetkn *datebsearch(char *key, datetkn *base, unsigned int nel);
extern bool AbsoluteTimeIsBefore(AbsoluteTime time1, AbsoluteTime time2);
extern bool AbsoluteTimeIsAfter(AbsoluteTime time1, AbsoluteTime time2);
extern int32 abstimeeq(AbsoluteTime t1, AbsoluteTime t2);
extern int32 abstimene(AbsoluteTime t1, AbsoluteTime t2);
extern int32 abstimelt(AbsoluteTime t1, AbsoluteTime t2);
extern int32 abstimegt(AbsoluteTime t1, AbsoluteTime t2);
extern int32 abstimele(AbsoluteTime t1, AbsoluteTime t2);
extern int32 abstimege(AbsoluteTime t1, AbsoluteTime t2);

#endif /* NABSTIME_H */
