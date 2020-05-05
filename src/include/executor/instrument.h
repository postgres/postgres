/*-------------------------------------------------------------------------
 *
 * instrument.h
 *	  definitions for run-time statistics collection
 *
 *
 * Copyright (c) 2001-2020, PostgreSQL Global Development Group
 *
 * src/include/executor/instrument.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INSTRUMENT_H
#define INSTRUMENT_H

#include "portability/instr_time.h"


typedef struct BufferUsage
{
	long		shared_blks_hit;	/* # of shared buffer hits */
	long		shared_blks_read;	/* # of shared disk blocks read */
	long		shared_blks_dirtied;	/* # of shared blocks dirtied */
	long		shared_blks_written;	/* # of shared disk blocks written */
	long		local_blks_hit; /* # of local buffer hits */
	long		local_blks_read;	/* # of local disk blocks read */
	long		local_blks_dirtied; /* # of shared blocks dirtied */
	long		local_blks_written; /* # of local disk blocks written */
	long		temp_blks_read; /* # of temp blocks read */
	long		temp_blks_written;	/* # of temp blocks written */
	instr_time	blk_read_time;	/* time spent reading */
	instr_time	blk_write_time; /* time spent writing */
} BufferUsage;

typedef struct WalUsage
{
	long		wal_records;	/* # of WAL records produced */
	long		wal_fpi;		/* # of WAL full page images produced */
	uint64		wal_bytes;		/* size of WAL records produced */
} WalUsage;

/* Flag bits included in InstrAlloc's instrument_options bitmask */
typedef enum InstrumentOption
{
	INSTRUMENT_TIMER = 1 << 0,	/* needs timer (and row counts) */
	INSTRUMENT_BUFFERS = 1 << 1,	/* needs buffer usage */
	INSTRUMENT_ROWS = 1 << 2,	/* needs row count */
	INSTRUMENT_WAL = 1 << 3,	/* needs WAL usage */
	INSTRUMENT_ALL = PG_INT32_MAX
} InstrumentOption;

typedef struct Instrumentation
{
	/* Parameters set at node creation: */
	bool		need_timer;		/* true if we need timer data */
	bool		need_bufusage;	/* true if we need buffer usage data */
	bool		need_walusage;	/* true if we need WAL usage data */
	/* Info about current plan cycle: */
	bool		running;		/* true if we've completed first tuple */
	instr_time	starttime;		/* start time of current iteration of node */
	instr_time	counter;		/* accumulated runtime for this node */
	double		firsttuple;		/* time for first tuple of this cycle */
	double		tuplecount;		/* # of tuples emitted so far this cycle */
	BufferUsage bufusage_start; /* buffer usage at start */
	WalUsage	walusage_start; /* WAL usage at start */
	/* Accumulated statistics across all completed cycles: */
	double		startup;		/* total startup time (in seconds) */
	double		total;			/* total time (in seconds) */
	double		ntuples;		/* total tuples produced */
	double		ntuples2;		/* secondary node-specific tuple counter */
	double		nloops;			/* # of run cycles for this node */
	double		nfiltered1;		/* # of tuples removed by scanqual or joinqual */
	double		nfiltered2;		/* # of tuples removed by "other" quals */
	BufferUsage bufusage;		/* total buffer usage */
	WalUsage	walusage;		/* total WAL usage */
} Instrumentation;

typedef struct WorkerInstrumentation
{
	int			num_workers;	/* # of structures that follow */
	Instrumentation instrument[FLEXIBLE_ARRAY_MEMBER];
} WorkerInstrumentation;

extern PGDLLIMPORT BufferUsage pgBufferUsage;
extern PGDLLIMPORT WalUsage pgWalUsage;

extern Instrumentation *InstrAlloc(int n, int instrument_options);
extern void InstrInit(Instrumentation *instr, int instrument_options);
extern void InstrStartNode(Instrumentation *instr);
extern void InstrStopNode(Instrumentation *instr, double nTuples);
extern void InstrEndLoop(Instrumentation *instr);
extern void InstrAggNode(Instrumentation *dst, Instrumentation *add);
extern void InstrStartParallelQuery(void);
extern void InstrEndParallelQuery(BufferUsage *bufusage, WalUsage *walusage);
extern void InstrAccumParallelQuery(BufferUsage *bufusage, WalUsage *walusage);
extern void BufferUsageAccumDiff(BufferUsage *dst,
								 const BufferUsage *add, const BufferUsage *sub);
extern void WalUsageAccumDiff(WalUsage *dst, const WalUsage *add,
							  const WalUsage *sub);

#endif							/* INSTRUMENT_H */
