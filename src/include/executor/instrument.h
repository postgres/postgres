/*-------------------------------------------------------------------------
 *
 * instrument.h
 *	  definitions for run-time statistics collection
 *
 *
 * Copyright (c) 2001-2003, PostgreSQL Global Development Group
 *
 * $Id: instrument.h,v 1.5 2003/08/04 23:59:41 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef INSTRUMENT_H
#define INSTRUMENT_H

#include <sys/time.h>


typedef struct Instrumentation
{
	/* Info about current plan cycle: */
	bool		running;		/* TRUE if we've completed first tuple */
	struct timeval starttime;	/* Start time of current iteration of node */
	struct timeval counter;		/* Accumulates runtime for this node */
	double		firsttuple;		/* Time for first tuple of this cycle */
	double		tuplecount;		/* Tuples so far this cycle */
	/* Accumulated statistics across all completed cycles: */
	double		startup;		/* Total startup time (in seconds) */
	double		total;			/* Total total time (in seconds) */
	double		ntuples;		/* Total tuples produced */
	double		nloops;			/* # of run cycles for this node */
} Instrumentation;

extern Instrumentation *InstrAlloc(void);
extern void InstrStartNode(Instrumentation *instr);
extern void InstrStopNode(Instrumentation *instr, bool returnedTuple);
extern void InstrEndLoop(Instrumentation *instr);

#endif   /* INSTRUMENT_H */
