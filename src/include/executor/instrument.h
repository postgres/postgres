/*-------------------------------------------------------------------------
 *
 * instrument.h
 *	  definitions for run-time statistics collection
 *
 *
 * Copyright (c) 2001-2010, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/executor/instrument.h,v 1.24 2010/02/26 02:01:24 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef INSTRUMENT_H
#define INSTRUMENT_H

#include "portability/instr_time.h"


typedef struct BufferUsage
{
	long		shared_blks_hit;	/* # of shared buffer hits */
	long		shared_blks_read;		/* # of shared disk blocks read */
	long		shared_blks_written;	/* # of shared disk blocks written */
	long		local_blks_hit; /* # of local buffer hits */
	long		local_blks_read;	/* # of local disk blocks read */
	long		local_blks_written;		/* # of local disk blocks written */
	long		temp_blks_read; /* # of temp blocks read */
	long		temp_blks_written;		/* # of temp blocks written */
} BufferUsage;

typedef enum InstrumentOption
{
	INSTRUMENT_TIMER = 1 << 0,	/* needs timer */
	INSTRUMENT_BUFFERS = 1 << 1,	/* needs buffer usage */
	INSTRUMENT_ALL = 0x7FFFFFFF
} InstrumentOption;

typedef struct Instrumentation
{
	/* Info about current plan cycle: */
	bool		running;		/* TRUE if we've completed first tuple */
	bool		needs_bufusage; /* TRUE if we need buffer usage */
	instr_time	starttime;		/* Start time of current iteration of node */
	instr_time	counter;		/* Accumulated runtime for this node */
	double		firsttuple;		/* Time for first tuple of this cycle */
	double		tuplecount;		/* Tuples emitted so far this cycle */
	BufferUsage bufusage_start; /* Buffer usage at start */
	/* Accumulated statistics across all completed cycles: */
	double		startup;		/* Total startup time (in seconds) */
	double		total;			/* Total total time (in seconds) */
	double		ntuples;		/* Total tuples produced */
	double		nloops;			/* # of run cycles for this node */
	BufferUsage bufusage;		/* Total buffer usage */
} Instrumentation;

extern PGDLLIMPORT BufferUsage pgBufferUsage;

extern Instrumentation *InstrAlloc(int n, int instrument_options);
extern void InstrStartNode(Instrumentation *instr);
extern void InstrStopNode(Instrumentation *instr, double nTuples);
extern void InstrEndLoop(Instrumentation *instr);

#endif   /* INSTRUMENT_H */
