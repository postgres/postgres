/*-------------------------------------------------------------------------
 *
 * nabstime.h--
 *	  Definitions for the "new" abstime code.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nabstime.h,v 1.12 1997/09/08 02:39:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NABSTIME_H
#define NABSTIME_H

#include <time.h>
#include "utils/dt.h"


/* ----------------------------------------------------------------
 *				time types + support macros
 *
 *
 * ----------------------------------------------------------------
 */
typedef int32 AbsoluteTime;
typedef int32 RelativeTime;

typedef struct
{
	int32		status;
	AbsoluteTime data[2];
}			TimeIntervalData;
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
#define INVALID_ABSTIME ((AbsoluteTime) 0x7FFFFFFE)		/* 2147483647 == 2^31 -
														 * 1 */
#define CURRENT_ABSTIME ((AbsoluteTime) 0x7FFFFFFD)		/* 2147483646 == 2^31 -
														 * 2 */
#define NOEND_ABSTIME	((AbsoluteTime) 0x7FFFFFFC)		/* 2147483645 == 2^31 -
														 * 3 */
#define BIG_ABSTIME		((AbsoluteTime) 0x7FFFFFFB)		/* 2147483644 == 2^31 -
														 * 4 */

#if defined(aix)
/*
 * AIX considers 2147483648 == -2147483648 (since they have the same bit
 * representation) but uses a different sign sense in a comparison to
 * these integer constants depending on whether the constant is signed
 * or not!
 */
#define NOSTART_ABSTIME		 ((AbsoluteTime) INT_MIN)
#else
#define NOSTART_ABSTIME ((AbsoluteTime) 0x80000001)		/* -2147483647 == - 2^31 */
#endif							/* aix */

#define INVALID_RELTIME ((RelativeTime) 0x7FFFFFFE)		/* 2147483647 == 2^31 -
														 * 1 */

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
 * getSystemTime --
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

extern AbsoluteTime datetime_abstime(DateTime * datetime);
extern DateTime *abstime_datetime(AbsoluteTime abstime);

extern bool AbsoluteTimeIsBefore(AbsoluteTime time1, AbsoluteTime time2);
extern bool AbsoluteTimeIsAfter(AbsoluteTime time1, AbsoluteTime time2);

extern void abstime2tm(AbsoluteTime time, int *tzp, struct tm * tm, char *tzn);

#endif							/* NABSTIME_H */
