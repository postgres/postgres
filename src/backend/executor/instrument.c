/*-------------------------------------------------------------------------
 *
 * instrument.c
 *	 functions for instrumentation of plan execution
 *
 *
 * Copyright (c) 2001-2006, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/instrument.c,v 1.15 2006/05/30 14:01:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <math.h>

#include "executor/instrument.h"

/* This is the function that is used to determine the sampling intervals. In
 * general, if the function is f(x), then for N tuples we will take on the
 * order of integral(1/f(x), x=0..N) samples. Some examples follow, with the
 * number of samples that would be collected over 1,000,000 tuples.

  f(x) = x         =>   log2(N)                        20
  f(x) = x^(1/2)   =>   2 * N^(1/2)                  2000
  f(x) = x^(1/3)   =>   1.5 * N^(2/3)               15000
  
 * I've chosen the last one as it seems to provide a good compromise between
 * low overhead but still getting a meaningful number of samples. However,
 * not all machines have the cbrt() function so on those we substitute
 * sqrt(). The difference is not very significant in the tests I made.
*/ 
#ifdef HAVE_CBRT
#define SampleFunc cbrt
#else
#define SampleFunc sqrt
#endif

#define SAMPLE_THRESHOLD 50

static double SampleOverhead;
static bool SampleOverheadCalculated;

static void
CalculateSampleOverhead()
{
	Instrumentation instr;
	int i;
	
	/* We want to determine the sampling overhead, to correct
	 * calculations later. This only needs to be done once per backend.
	 * Is this the place? A wrong value here (due to a mistimed
	 * task-switch) will cause bad calculations later.
	 *
	 * To minimize the risk we do it a few times and take the lowest.
	 */
	
	SampleOverhead = 1.0e6;
	
	for( i = 0; i<5; i++ )
	{
		int j;
		double overhead;
		
		memset( &instr, 0, sizeof(instr) );
	
		/* Loop SAMPLE_THRESHOLD times or 100 microseconds, whichever is faster */
		for( j=0; j<SAMPLE_THRESHOLD && INSTR_TIME_GET_DOUBLE(instr.counter) < 100e-6; i++ )
		{
			InstrStartNode( &instr );
			InstrStopNode( &instr, 1 );
		}
		overhead = INSTR_TIME_GET_DOUBLE(instr.counter) / instr.samplecount;
		if( overhead < SampleOverhead )
			SampleOverhead = overhead;
	}
		
	SampleOverheadCalculated = true;
}

/* Allocate new instrumentation structure(s) */
Instrumentation *
InstrAlloc(int n)
{
	Instrumentation *instr = palloc0(n * sizeof(Instrumentation));

	/* we don't need to do any initialization except zero 'em */
	
	/* Calculate overhead, if not done yet */
	if( !SampleOverheadCalculated )
		CalculateSampleOverhead();
	return instr;
}

/* Entry to a plan node */
void
InstrStartNode(Instrumentation *instr)
{
	if (INSTR_TIME_IS_ZERO(instr->starttime))
	{
		/* We always sample the first SAMPLE_THRESHOLD tuples, so small nodes are always accurate */
		if (instr->tuplecount < SAMPLE_THRESHOLD)
			instr->sampling = true;
		else
		{
			/* Otherwise we go to sampling, see the comments on SampleFunc at the top of the file */
			if( instr->tuplecount > instr->nextsample )
			{
				instr->sampling = true;
				/* The doubling is so the random will average 1 over time */
				instr->nextsample += 2.0 * SampleFunc(instr->tuplecount) * (double)rand() / (double)RAND_MAX;
			}
		}
		if (instr->sampling)
			INSTR_TIME_SET_CURRENT(instr->starttime);
	}
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

	if (instr->sampling)
	{
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
		instr->samplecount += nTuples;
		instr->sampling = false;
	}

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

	/* Here we take into account sampling effects. Doing it naively ends
	 * up assuming the sampling overhead applies to all tuples, even the
	 * ones we didn't measure. We've calculated an overhead, so we
	 * subtract that for all samples we didn't measure. The first tuple
	 * is also special cased, because it usually takes longer. */

	if( instr->samplecount < instr->tuplecount )
	{
		double pertuple = (totaltime - instr->firsttuple) / (instr->samplecount - 1);
		instr->total += instr->firsttuple + (pertuple * (instr->samplecount - 1))
		                                  + ((pertuple - SampleOverhead) * (instr->tuplecount - instr->samplecount));
	}
	else
		instr->total += totaltime;
		
	instr->ntuples += instr->tuplecount;
	instr->nsamples += instr->samplecount;
	instr->nloops += 1;

	/* Reset for next cycle (if any) */
	instr->running = false;
	INSTR_TIME_SET_ZERO(instr->starttime);
	INSTR_TIME_SET_ZERO(instr->counter);
	instr->firsttuple = 0;
	instr->samplecount = 0;
	instr->tuplecount = 0;
}
