/*-------------------------------------------------------------------------
 *
 * datetime.h--
 *    Definitions for the datetime
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: datetime.h,v 1.2 1997/03/14 23:33:21 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DATETIME_H
#define DATETIME_H

#include "utils/dt.h"

#if USE_NEW_DATE

typedef int32	DateADT;

#else

/* these things look like structs, but we pass them by value so be careful
   For example, passing an int -> DateADT is not portable! */
typedef struct DateADT {
    char	day;
    char	month;
    short	year;
} DateADT;

#endif

#if USE_NEW_TIME

typedef float8	TimeADT;

#else

typedef struct TimeADT {
    short	hr;
    short	min;
    float	sec;
} TimeADT;

#endif

#endif /* DATETIME_H */
