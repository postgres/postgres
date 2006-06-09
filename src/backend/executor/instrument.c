/*-------------------------------------------------------------------------
 *
 * instrument.c
 *	 functions for instrumentation of plan execution
 *
 *
 * Copyright (c) 2001-2006, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/instrument.c,v 1.18 2006/06/09 19:30:56 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "executor/instrument.h"


/* Allocate new instrumentation structure(s) */
Instrumentation *
InstrAlloc(int n)
{
	Instrumentation *instr = palloc0(n * sizeof(Instrumentation));

	/* we don't need to do any initialization except zero 'em */

	return instr;
}

/* Entry to a plan node */
void
InstrStartNode(Instrumentation *instr)
{
	if (INSTR_TIME_IS_ZERO(instr->starttime))
		INSTR_TIME_SET_CURRENT(instr->starttime);
	else
		elog(DEBUG2, "InstrStartNode called twice in a row");
}

/* Exit from a plan node */
void
InstrStopNode(Instrumentation *instr, double nTuples)
{
	instr_time	endtime;

	/* count the returned tuples */
	instr->tuplecount += nTuples;

	if (INSTR_TIME_IS_ZERO(instr->starttime))
	{
		elog(DEBUG2, "InstrStopNode called without start");
		return;
	}

	INSTR_TIME_SET_CURRENT(endtime);

#ifndef WIN32
	instr->counter.tv_sec += endtime.tv_sec - instr->starttime.tv_sec;
	instr->counter.tv_usec += endtime.tv_usec - instr->starttime.tv_usec;

	/* Normalize after each add to avoid overflow/underflow of tv_usec */
	while (instr->counter.tv_usec < 0)
	{
		instr->counter.tv_usec += 1000000;
		instr->counter.tv_sec--;
	}
	while (instr->counter.tv_usec >= 1000000)
	{
		instr->counter.tv_usec -= 1000000;
		instr->counter.tv_sec++;
	}
#else							/* WIN32 */
	instr->counter.QuadPart += (endtime.QuadPart - instr->starttime.QuadPart);
#endif

	INSTR_TIME_SET_ZERO(instr->starttime);

	/* Is this the first tuple of this cycle? */
	if (!instr->running)
	{
		instr->running = true;
		instr->firsttuple = INSTR_TIME_GET_DOUBLE(instr->counter);
	}
}

/* Finish a run cycle for a plan node */
void
InstrEndLoop(Instrumentation *instr)
{
	double		totaltime;

	/* Skip if nothing has happened, or already shut down */
	if (!instr->running)
		return;

	if (!INSTR_TIME_IS_ZERO(instr->starttime))
		elog(DEBUG2, "InstrEndLoop called on running node");

	/* Accumulate per-cycle statistics into totals */
	totaltime = INSTR_TIME_GET_DOUBLE(instr->counter);

	instr->startup += instr->firsttuple;
	instr->total += totaltime;
	instr->ntuples += instr->tuplecount;
	instr->nloops += 1;

	/* Reset for next cycle (if any) */
	instr->running = false;
	INSTR_TIME_SET_ZERO(instr->starttime);
	INSTR_TIME_SET_ZERO(instr->counter);
	instr->firsttuple = 0;
	instr->tuplecount = 0;
}
