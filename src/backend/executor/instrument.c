/*-------------------------------------------------------------------------
 *
 * instrument.c
 *	 functions for instrumentation of plan execution
 *
 *
 * Copyright (c) 2001-2006, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/instrument.c,v 1.17 2006/06/07 18:49:03 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <unistd.h>

#include "executor/instrument.h"


/*
 * As of PostgreSQL 8.2, we try to reduce the overhead of EXPLAIN ANALYZE
 * by not calling INSTR_TIME_SET_CURRENT() for every single node execution.
 * (Because that requires a kernel call on most systems, it's expensive.)
 *
 * This macro determines the sampling interval: that is, after how many more
 * iterations will we take the next time sample, given that niters iterations
 * have occurred already.  In general, if the function is f(x), then for N
 * iterations we will take on the order of integral(1/f(x), x=0..N)
 * samples.  Some examples follow, with the number of samples that would be
 * collected over 1,000,000 iterations:
 *
 *		f(x) = x         =>   log2(N)                        20
 *		f(x) = x^(1/2)   =>   2 * N^(1/2)                  2000
 *		f(x) = x^(1/3)   =>   1.5 * N^(2/3)               15000
 *
 * I've chosen the last one as it seems to provide a good compromise between
 * low overhead but still getting a meaningful number of samples. However,
 * not all machines have the cbrt() function so on those we substitute
 * sqrt(). The difference is not very significant in the tests I made.
 *
 * The actual sampling interval is randomized with the SampleFunc() value
 * as the mean; this hopefully will reduce any measurement bias due to
 * variation in the node execution time.
 */
#ifdef HAVE_CBRT
#define SampleFunc(niters) cbrt(niters)
#else
#define SampleFunc(niters) sqrt(niters)
#endif

#define SampleInterval(niters) \
	(SampleFunc(niters) * (double) random() / (double) (MAX_RANDOM_VALUE/2))

/*
 * We sample at every node iteration until we've reached this threshold,
 * so that nodes not called a large number of times are completely accurate.
 * (Perhaps this should be a GUC variable?)
 */
#define SAMPLE_THRESHOLD 50

/*
 * Determine the sampling overhead.  This only needs to be done once per
 * backend (actually, probably once per postmaster would do ...)
 */
static double SampleOverhead;
static bool SampleOverheadCalculated = false;

static void
CalculateSampleOverhead(void)
{
	int i;

	/*
	 * We could get a wrong result due to being interrupted by task switch.
	 * To minimize the risk we do it a few times and take the lowest.
	 */
	SampleOverhead = 1.0e6;

	for (i = 0; i < 5; i++)
	{
		Instrumentation timer;
		instr_time	tmptime;
		int j;
		double overhead;

		memset(&timer, 0, sizeof(timer));
		InstrStartNode(&timer);
#define TEST_COUNT 100
		for (j = 0; j < TEST_COUNT; j++)
		{
			INSTR_TIME_SET_CURRENT(tmptime);
		}
		InstrStopNode(&timer, 1);
		overhead = INSTR_TIME_GET_DOUBLE(timer.counter) / TEST_COUNT;
		if (overhead < SampleOverhead)
			SampleOverhead = overhead;
	}

	SampleOverheadCalculated = true;
}


/* Allocate new instrumentation structure(s) */
Instrumentation *
InstrAlloc(int n)
{
	Instrumentation *instr;

	/* Calculate sampling overhead, if not done yet in this backend */
	if (!SampleOverheadCalculated)
		CalculateSampleOverhead();

	instr = palloc0(n * sizeof(Instrumentation));

	/* we don't need to do any initialization except zero 'em */

	return instr;
}

/* Entry to a plan node */
void
InstrStartNode(Instrumentation *instr)
{
	if (INSTR_TIME_IS_ZERO(instr->starttime))
	{
		/*
		 * Always sample if not yet up to threshold, else check whether
		 * next threshold has been reached
		 */
		if (instr->itercount < SAMPLE_THRESHOLD)
			instr->sampling = true;
		else if (instr->itercount >= instr->nextsample)
		{
			instr->sampling = true;
			instr->nextsample =
				instr->itercount + SampleInterval(instr->itercount);
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
	/* count the returned tuples and iterations */
	instr->tuplecount += nTuples;
	instr->itercount += 1;

	/* measure runtime if appropriate */
	if (instr->sampling)
	{
		instr_time	endtime;

		/*
		 * To be sure that SampleOverhead accurately reflects the extra
		 * overhead, we must do INSTR_TIME_SET_CURRENT() as the *first*
		 * action that is different between the sampling and non-sampling
		 * code paths.
		 */
		INSTR_TIME_SET_CURRENT(endtime);

		if (INSTR_TIME_IS_ZERO(instr->starttime))
		{
			elog(DEBUG2, "InstrStopNode called without start");
			return;
		}

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

		instr->samplecount += 1;
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

	/* Compute time spent in node */
	totaltime = INSTR_TIME_GET_DOUBLE(instr->counter);

	/*
	 * If we didn't measure runtime on every iteration, then we must increase
	 * the measured total to account for the other iterations.  Naively
	 * multiplying totaltime by itercount/samplecount would be wrong because
	 * it effectively assumes the sampling overhead applies to all iterations,
	 * even the ones we didn't measure.  (Note that what we are trying to
	 * estimate here is the actual time spent in the node, including the
	 * actual measurement overhead; not the time exclusive of measurement
	 * overhead.)  We exclude the first iteration from the correction basis,
	 * because it often takes longer than others.
	 */
	if (instr->itercount > instr->samplecount)
	{
		double per_iter;

		per_iter = (totaltime - instr->firsttuple) / (instr->samplecount - 1)
			- SampleOverhead;
		if (per_iter > 0)				/* sanity check */
			totaltime += per_iter * (instr->itercount - instr->samplecount);
	}

	/* Accumulate per-cycle statistics into totals */
	instr->startup += instr->firsttuple;
	instr->total += totaltime;
	instr->ntuples += instr->tuplecount;
	instr->nloops += 1;

	/* Reset for next cycle (if any) */
	instr->running = false;
	instr->sampling = false;
	INSTR_TIME_SET_ZERO(instr->starttime);
	INSTR_TIME_SET_ZERO(instr->counter);
	instr->firsttuple = 0;
	instr->tuplecount = 0;
	instr->itercount = 0;
	instr->samplecount = 0;
	instr->nextsample = 0;
}
