/*-------------------------------------------------------------------------
 *
 * tqual.h--
 *	  POSTGRES time qualification definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tqual.h,v 1.7 1997/09/07 05:03:03 momjian Exp $
 *
 * NOTE
 *	  It may be desirable to allow time qualifications to indicate
 *	  relative times.
 *
 *-------------------------------------------------------------------------
 */
#ifndef TQUAL_H
#define TQUAL_H

#include <access/htup.h>

typedef struct TimeQualSpace
{
	char			data[12];
}				TimeQualSpace;

typedef Pointer TimeQual;

/* Tuples valid as of StartTransactionCommand */
#define NowTimeQual		((TimeQual) NULL)

/* As above, plus updates in this command */
extern TimeQual SelfTimeQual;

extern void		setheapoverride(bool on);
extern bool		heapisoverride(void);

extern TimeQual TimeFormSnapshotTimeQual(AbsoluteTime time);
extern TimeQual
TimeFormRangedTimeQual(AbsoluteTime startTime,
					   AbsoluteTime endTime);
extern bool		HeapTupleSatisfiesTimeQual(HeapTuple tuple, TimeQual qual);


#endif							/* TQUAL_H */
