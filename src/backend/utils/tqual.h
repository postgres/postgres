/*-------------------------------------------------------------------------
 *
 * tqual.h--
 *    POSTGRES time qualification definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tqual.h,v 1.1.1.1 1996/07/09 06:22:02 scrappy Exp $
 *
 * NOTE
 *    It may be desirable to allow time qualifications to indicate
 *    relative times.
 *
 *-------------------------------------------------------------------------
 */
#ifndef	TQUAL_H
#define TQUAL_H

#include "postgres.h"
#include "utils/nabstime.h"
#include "access/htup.h"

typedef struct TimeQualSpace {
    char	data[12];
} TimeQualSpace;

typedef Pointer	TimeQual;

/* Tuples valid as of StartTransactionCommand */
#define	NowTimeQual	((TimeQual) NULL)

/* As above, plus updates in this command */
extern TimeQual	SelfTimeQual;

extern void setheapoverride(bool on);
extern bool heapisoverride(void);

extern bool TimeQualIsValid(TimeQual qual);
extern bool TimeQualIsLegal(TimeQual qual);
extern bool TimeQualIncludesNow(TimeQual qual);
extern bool TimeQualIncludesPast(TimeQual qual);
extern bool TimeQualIsSnapshot(TimeQual qual);
extern bool TimeQualIsRanged(TimeQual qual);
extern bool TimeQualIndicatesDisableValidityChecking(TimeQual qual);
extern AbsoluteTime TimeQualGetSnapshotTime(TimeQual qual);
extern AbsoluteTime TimeQualGetStartTime(TimeQual qual);
extern AbsoluteTime TimeQualGetEndTime(TimeQual qual);
extern TimeQual TimeFormSnapshotTimeQual(AbsoluteTime time);
extern TimeQual TimeFormRangedTimeQual(AbsoluteTime startTime,
				       AbsoluteTime endTime);
extern bool HeapTupleSatisfiesTimeQual(HeapTuple tuple, TimeQual qual);


#endif	/* TQUAL_H */
