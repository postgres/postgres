/*-------------------------------------------------------------------------
 *
 * nabstime.h
 *	  Definitions for the "new" abstime code.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nabstime.h,v 1.40 2003/08/04 02:40:15 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NABSTIME_H
#define NABSTIME_H

#include <limits.h>
#include <time.h>

#include "fmgr.h"
#include "utils/timestamp.h"
#include "utils/datetime.h"


/* ----------------------------------------------------------------
 *
 *				time types + support macros
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
 * Macros for fmgr-callable functions.
 */
#define DatumGetAbsoluteTime(X)  ((AbsoluteTime) DatumGetInt32(X))
#define DatumGetRelativeTime(X)  ((RelativeTime) DatumGetInt32(X))
#define DatumGetTimeInterval(X)  ((TimeInterval) DatumGetPointer(X))

#define AbsoluteTimeGetDatum(X)  Int32GetDatum(X)
#define RelativeTimeGetDatum(X)  Int32GetDatum(X)
#define TimeIntervalGetDatum(X)  PointerGetDatum(X)

#define PG_GETARG_ABSOLUTETIME(n)  DatumGetAbsoluteTime(PG_GETARG_DATUM(n))
#define PG_GETARG_RELATIVETIME(n)  DatumGetRelativeTime(PG_GETARG_DATUM(n))
#define PG_GETARG_TIMEINTERVAL(n)  DatumGetTimeInterval(PG_GETARG_DATUM(n))

#define PG_RETURN_ABSOLUTETIME(x)  return AbsoluteTimeGetDatum(x)
#define PG_RETURN_RELATIVETIME(x)  return RelativeTimeGetDatum(x)
#define PG_RETURN_TIMEINTERVAL(x)  return TimeIntervalGetDatum(x)

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
#define INVALID_ABSTIME ((AbsoluteTime) 0x7FFFFFFE)		/* 2147483647 (2^31 - 1) */
#define NOEND_ABSTIME	((AbsoluteTime) 0x7FFFFFFC)		/* 2147483645 (2^31 - 3) */
#define NOSTART_ABSTIME ((AbsoluteTime) INT_MIN)		/* -2147483648 */

#define INVALID_RELTIME ((RelativeTime) 0x7FFFFFFE)		/* 2147483647 (2^31 - 1) */

#define AbsoluteTimeIsValid(time) \
	((bool) ((time) != INVALID_ABSTIME))

/*
 * Because NOSTART_ABSTIME is defined as INT_MIN, there can't be any
 * AbsoluteTime values less than it.  Therefore, we can code the test
 * "time > NOSTART_ABSTIME" as "time != NOSTART_ABSTIME", which avoids
 * compiler bugs on some platforms.  --- tgl & az, 11/2000
 */
#define AbsoluteTimeIsReal(time) \
	((bool) (((AbsoluteTime) (time)) < NOEND_ABSTIME && \
			  ((AbsoluteTime) (time)) != NOSTART_ABSTIME))

#define RelativeTimeIsValid(time) \
	((bool) (((RelativeTime) (time)) != INVALID_RELTIME))


/*
 * nabstime.c prototypes
 */
extern Datum abstimein(PG_FUNCTION_ARGS);
extern Datum abstimeout(PG_FUNCTION_ARGS);
extern Datum abstimerecv(PG_FUNCTION_ARGS);
extern Datum abstimesend(PG_FUNCTION_ARGS);

extern Datum abstimeeq(PG_FUNCTION_ARGS);
extern Datum abstimene(PG_FUNCTION_ARGS);
extern Datum abstimelt(PG_FUNCTION_ARGS);
extern Datum abstimegt(PG_FUNCTION_ARGS);
extern Datum abstimele(PG_FUNCTION_ARGS);
extern Datum abstimege(PG_FUNCTION_ARGS);
extern Datum abstime_finite(PG_FUNCTION_ARGS);

extern Datum timestamp_abstime(PG_FUNCTION_ARGS);
extern Datum abstime_timestamp(PG_FUNCTION_ARGS);
extern Datum timestamptz_abstime(PG_FUNCTION_ARGS);
extern Datum abstime_timestamptz(PG_FUNCTION_ARGS);

extern Datum reltimein(PG_FUNCTION_ARGS);
extern Datum reltimeout(PG_FUNCTION_ARGS);
extern Datum reltimerecv(PG_FUNCTION_ARGS);
extern Datum reltimesend(PG_FUNCTION_ARGS);
extern Datum tintervalin(PG_FUNCTION_ARGS);
extern Datum tintervalout(PG_FUNCTION_ARGS);
extern Datum tintervalrecv(PG_FUNCTION_ARGS);
extern Datum tintervalsend(PG_FUNCTION_ARGS);
extern Datum interval_reltime(PG_FUNCTION_ARGS);
extern Datum reltime_interval(PG_FUNCTION_ARGS);
extern Datum mktinterval(PG_FUNCTION_ARGS);
extern Datum timepl(PG_FUNCTION_ARGS);
extern Datum timemi(PG_FUNCTION_ARGS);

extern Datum intinterval(PG_FUNCTION_ARGS);
extern Datum tintervalrel(PG_FUNCTION_ARGS);
extern Datum timenow(PG_FUNCTION_ARGS);
extern Datum reltimeeq(PG_FUNCTION_ARGS);
extern Datum reltimene(PG_FUNCTION_ARGS);
extern Datum reltimelt(PG_FUNCTION_ARGS);
extern Datum reltimegt(PG_FUNCTION_ARGS);
extern Datum reltimele(PG_FUNCTION_ARGS);
extern Datum reltimege(PG_FUNCTION_ARGS);
extern Datum tintervalsame(PG_FUNCTION_ARGS);
extern Datum tintervaleq(PG_FUNCTION_ARGS);
extern Datum tintervalne(PG_FUNCTION_ARGS);
extern Datum tintervallt(PG_FUNCTION_ARGS);
extern Datum tintervalgt(PG_FUNCTION_ARGS);
extern Datum tintervalle(PG_FUNCTION_ARGS);
extern Datum tintervalge(PG_FUNCTION_ARGS);
extern Datum tintervalleneq(PG_FUNCTION_ARGS);
extern Datum tintervallenne(PG_FUNCTION_ARGS);
extern Datum tintervallenlt(PG_FUNCTION_ARGS);
extern Datum tintervallengt(PG_FUNCTION_ARGS);
extern Datum tintervallenle(PG_FUNCTION_ARGS);
extern Datum tintervallenge(PG_FUNCTION_ARGS);
extern Datum tintervalct(PG_FUNCTION_ARGS);
extern Datum tintervalov(PG_FUNCTION_ARGS);
extern Datum tintervalstart(PG_FUNCTION_ARGS);
extern Datum tintervalend(PG_FUNCTION_ARGS);
extern Datum timeofday(PG_FUNCTION_ARGS);

/* non-fmgr-callable support routines */
extern AbsoluteTime GetCurrentAbsoluteTime(void);
extern AbsoluteTime GetCurrentAbsoluteTimeUsec(int *usec);
extern TimestampTz AbsoluteTimeUsecToTimestampTz(AbsoluteTime sec, int usec);
extern void abstime2tm(AbsoluteTime time, int *tzp, struct tm * tm, char **tzn);

#endif   /* NABSTIME_H */
