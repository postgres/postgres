/*-------------------------------------------------------------------------
 *
 * nabstime.h
 *	  Definitions for the "new" abstime code.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nabstime.h,v 1.24 2000/03/27 17:07:48 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NABSTIME_H
#define NABSTIME_H

#include <time.h>
#include "utils/timestamp.h"
#include "utils/datetime.h"


/* ----------------------------------------------------------------
 *				time types + support macros
 *
 *
 * ----------------------------------------------------------------
 */
/* 
 * Although time_t generally is a long int on 64 bit systems, these two
 * types must be 4 bytes, because that's what the system assumes. They
 * should be yanked (long) before 2038 and be replaced by timestamp and
 * interval.
 */
typedef int32 AbsoluteTime;
typedef int32 RelativeTime;

typedef struct
{
	int32		status;
	AbsoluteTime data[2];
} TimeIntervalData;
typedef TimeIntervalData *TimeInterval;

/*
 * Reserved values
 * Epoch is Unix system time zero, but needs to be kept as a reserved
 *	value rather than converting to time since timezone calculations
 *	might move it away from 1970-01-01 00:00:00Z - tgl 97/02/20
 *
 * Pre-v6.1 code had large decimal numbers for reserved values.
 * These were chosen as special 32-bit bit patterns,
 *	so redefine them explicitly using these bit patterns. - tgl 97/02/24
 */
#define EPOCH_ABSTIME	((AbsoluteTime) 0)
#define INVALID_ABSTIME ((AbsoluteTime) 0x7FFFFFFE)	 /* 2147483647 (2^31 - 1) */
#define CURRENT_ABSTIME ((AbsoluteTime) 0x7FFFFFFD)	 /* 2147483646 (2^31 - 2) */
#define NOEND_ABSTIME	((AbsoluteTime) 0x7FFFFFFC)	 /* 2147483645 (2^31 - 3) */
#define BIG_ABSTIME		((AbsoluteTime) 0x7FFFFFFB)	 /* 2147483644 (2^31 - 4) */

#if defined(_AIX)
/*
 * AIX considers 2147483648 == -2147483648 (since they have the same bit
 * representation) but uses a different sign sense in a comparison to
 * these integer constants depending on whether the constant is signed
 * or not!
 */
#define NOSTART_ABSTIME		 ((AbsoluteTime) INT_MIN)
#else
#define NOSTART_ABSTIME ((AbsoluteTime) 0x80000001)	 /* -2147483647 (- 2^31) */
#endif	 /* _AIX */

#define INVALID_RELTIME ((RelativeTime) 0x7FFFFFFE)	 /* 2147483647 (2^31 - 1) */

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

extern AbsoluteTime GetCurrentAbsoluteTime(void);

/*
 * getSystemTime
 *		Returns system time.
 */
#define getSystemTime() \
	((time_t) (time(0l)))


/*
 * nabstime.c prototypes
 */
extern AbsoluteTime nabstimein(char *timestr);
extern char *nabstimeout(AbsoluteTime time);

extern bool abstimeeq(AbsoluteTime t1, AbsoluteTime t2);
extern bool abstimene(AbsoluteTime t1, AbsoluteTime t2);
extern bool abstimelt(AbsoluteTime t1, AbsoluteTime t2);
extern bool abstimegt(AbsoluteTime t1, AbsoluteTime t2);
extern bool abstimele(AbsoluteTime t1, AbsoluteTime t2);
extern bool abstimege(AbsoluteTime t1, AbsoluteTime t2);
extern bool abstime_finite(AbsoluteTime time);

extern AbsoluteTime timestamp_abstime(Timestamp *timestamp);
extern Timestamp *abstime_timestamp(AbsoluteTime abstime);

extern bool AbsoluteTimeIsBefore(AbsoluteTime time1, AbsoluteTime time2);

extern void abstime2tm(AbsoluteTime time, int *tzp, struct tm * tm, char *tzn);

extern RelativeTime reltimein(char *timestring);
extern char *reltimeout(RelativeTime timevalue);
extern TimeInterval tintervalin(char *intervalstr);
extern char *tintervalout(TimeInterval interval);
extern RelativeTime interval_reltime(Interval *interval);
extern Interval *reltime_interval(RelativeTime reltime);
extern TimeInterval mktinterval(AbsoluteTime t1, AbsoluteTime t2);
extern AbsoluteTime timepl(AbsoluteTime t1, RelativeTime t2);
extern AbsoluteTime timemi(AbsoluteTime t1, RelativeTime t2);

/* extern RelativeTime abstimemi(AbsoluteTime t1, AbsoluteTime t2);  static*/
extern int	intinterval(AbsoluteTime t, TimeInterval interval);
extern RelativeTime tintervalrel(TimeInterval interval);
extern AbsoluteTime timenow(void);
extern bool reltimeeq(RelativeTime t1, RelativeTime t2);
extern bool reltimene(RelativeTime t1, RelativeTime t2);
extern bool reltimelt(RelativeTime t1, RelativeTime t2);
extern bool reltimegt(RelativeTime t1, RelativeTime t2);
extern bool reltimele(RelativeTime t1, RelativeTime t2);
extern bool reltimege(RelativeTime t1, RelativeTime t2);
extern bool tintervalsame(TimeInterval i1, TimeInterval i2);
extern bool tintervaleq(TimeInterval i1, TimeInterval i2);
extern bool tintervalne(TimeInterval i1, TimeInterval i2);
extern bool tintervallt(TimeInterval i1, TimeInterval i2);
extern bool tintervalgt(TimeInterval i1, TimeInterval i2);
extern bool tintervalle(TimeInterval i1, TimeInterval i2);
extern bool tintervalge(TimeInterval i1, TimeInterval i2);
extern bool tintervalleneq(TimeInterval i, RelativeTime t);
extern bool tintervallenne(TimeInterval i, RelativeTime t);
extern bool tintervallenlt(TimeInterval i, RelativeTime t);
extern bool tintervallengt(TimeInterval i, RelativeTime t);
extern bool tintervallenle(TimeInterval i, RelativeTime t);
extern bool tintervallenge(TimeInterval i, RelativeTime t);
extern bool tintervalct(TimeInterval i1, TimeInterval i2);
extern bool tintervalov(TimeInterval i1, TimeInterval i2);
extern AbsoluteTime tintervalstart(TimeInterval i);
extern AbsoluteTime tintervalend(TimeInterval i);
extern int32 int4reltime(int32 timevalue);
extern text *timeofday(void);

#endif	 /* NABSTIME_H */
