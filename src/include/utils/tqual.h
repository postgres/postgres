/*-------------------------------------------------------------------------
 *
 * tqual.h--
 *	  POSTGRES "time" qualification definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tqual.h,v 1.10 1997/11/02 15:27:14 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TQUAL_H
#define TQUAL_H

#include <access/htup.h>

typedef Pointer TimeQual;

/* Tuples valid as of StartTransactionCommand */
#define NowTimeQual		((TimeQual) NULL)

/* As above, plus updates in this command */
extern TimeQual SelfTimeQual;

extern void setheapoverride(bool on);
extern bool heapisoverride(void);

extern bool HeapTupleSatisfiesTimeQual(HeapTuple tuple, TimeQual qual);


#endif							/* TQUAL_H */
