/*-------------------------------------------------------------------------
 *
 * instrument.c
 *	 functions for instrumentation of plan execution
 *
 *
 * Copyright (c) 2001-2005, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/instrument.c,v 1.10 2005/03/20 22:27:51 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "executor/instrument.h"


/* Allocate new instrumentation structure */
Instrumentation *
InstrAlloc(void)
{
	Instrumentation *instr = palloc(sizeof(Instrumentation));

	memset(instr, 0, sizeof(Instrumentation));

	return instr;
}

/* Entry to a plan node */
void
InstrStartNode(Instrumentation *instr)
{
	if (!instr)
		return;

	if (!INSTR_TIME_IS_ZERO(instr->starttime))
		elog(DEBUG2, "InstrStartNode called twice in a row");
	else
		INSTR_TIME_SET_CURRENT(instr->starttime);
}

/* Exit from a plan node */
void
InstrStopNode(Instrumentation *instr, bool returnedTuple)
{
	instr_time endtime;

	if (!instr)
		return;

	if (INSTR_TIME_IS_ZERO(instr->starttime))
	{
		elog(DEBUG2, "InstrStopNode without start");
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
#else /* WIN32 */
	instr->counter.QuadPart += (endtime.QuadPart - instr->starttime.QuadPart);
#endif

	INSTR_TIME_SET_ZERO(instr->starttime);

	/* Is this the first tuple of this cycle? */
	if (!instr->running)
	{
		instr->running = true;
		instr->firsttuple = INSTR_TIME_GET_DOUBLE(instr->counter);
	}

	if (returnedTuple)
		instr->tuplecount += 1;
}

/* Finish a run cycle for a plan node */
void
InstrEndLoop(Instrumentation *instr)
{
	double		totaltime;

	if (!instr)
		return;

	/* Skip if nothing has happened, or already shut down */
	if (!instr->running)
		return;

	/* Accumulate statistics */
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
