/*-------------------------------------------------------------------------
 *
 * instrument.c
 *	 functions for instrumentation of plan execution
 *
 *
 * Copyright (c) 2001-2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/instrument.c,v 1.5 2003/08/04 23:59:37 tgl Exp $
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

	if (instr->starttime.tv_sec != 0 || instr->starttime.tv_usec != 0)
		elog(DEBUG2, "InstrStartTimer called twice in a row");
	else
		gettimeofday(&instr->starttime, NULL);
}

/* Exit from a plan node */
void
InstrStopNode(Instrumentation *instr, bool returnedTuple)
{
	struct timeval endtime;

	if (!instr)
		return;

	if (instr->starttime.tv_sec == 0 && instr->starttime.tv_usec == 0)
	{
		elog(DEBUG2, "InstrStopNode without start");
		return;
	}

	gettimeofday(&endtime, NULL);

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

	instr->starttime.tv_sec = 0;
	instr->starttime.tv_usec = 0;

	/* Is this the first tuple of this cycle? */
	if (!instr->running)
	{
		instr->running = true;
		instr->firsttuple = (double) instr->counter.tv_sec +
			(double) instr->counter.tv_usec / 1000000.0;
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
	totaltime = (double) instr->counter.tv_sec +
		(double) instr->counter.tv_usec / 1000000.0;

	instr->startup += instr->firsttuple;
	instr->total += totaltime;
	instr->ntuples += instr->tuplecount;
	instr->nloops += 1;

	/* Reset for next cycle (if any) */
	instr->running = false;
	instr->starttime.tv_sec = 0;
	instr->starttime.tv_usec = 0;
	instr->counter.tv_sec = 0;
	instr->counter.tv_usec = 0;
	instr->firsttuple = 0;
	instr->tuplecount = 0;
}
