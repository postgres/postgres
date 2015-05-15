/*-------------------------------------------------------------------------
 *
 * tsm_system_time.c
 *	  interface routines for system_time tablesample method
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/tsm_system_time_rowlimit/tsm_system_time.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"

#include "access/tablesample.h"
#include "access/relscan.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/relation.h"
#include "optimizer/clauses.h"
#include "storage/bufmgr.h"
#include "utils/sampling.h"
#include "utils/spccache.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC;

/*
 * State
 */
typedef struct
{
	SamplerRandomState randstate;
	uint32			seed;			/* random seed */
	BlockNumber		nblocks;		/* number of block in relation */
	int32			time;			/* time limit for sampling */
	TimestampTz		start_time;		/* start time of sampling */
	TimestampTz		end_time;		/* end time of sampling */
	OffsetNumber	lt;				/* last tuple returned from current block */
	BlockNumber		step;			/* step size */
	BlockNumber		lb;				/* last block visited */
	BlockNumber		estblocks;		/* estimated number of returned blocks (moving) */
	BlockNumber		doneblocks;		/* number of already returned blocks */
} SystemSamplerData;


PG_FUNCTION_INFO_V1(tsm_system_time_init);
PG_FUNCTION_INFO_V1(tsm_system_time_nextblock);
PG_FUNCTION_INFO_V1(tsm_system_time_nexttuple);
PG_FUNCTION_INFO_V1(tsm_system_time_end);
PG_FUNCTION_INFO_V1(tsm_system_time_reset);
PG_FUNCTION_INFO_V1(tsm_system_time_cost);

static uint32 random_relative_prime(uint32 n, SamplerRandomState randstate);

/*
 * Initializes the state.
 */
Datum
tsm_system_time_init(PG_FUNCTION_ARGS)
{
	TableSampleDesc	   *tsdesc = (TableSampleDesc *) PG_GETARG_POINTER(0);
	uint32				seed = PG_GETARG_UINT32(1);
	int32				time = PG_ARGISNULL(2) ? -1 : PG_GETARG_INT32(2);
	HeapScanDesc		scan = tsdesc->heapScan;
	SystemSamplerData  *sampler;

	if (time < 1)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("invalid time limit"),
				 errhint("Time limit must be positive integer value.")));

	sampler = palloc0(sizeof(SystemSamplerData));

	/* Remember initial values for reinit */
	sampler->seed = seed;
	sampler->nblocks = scan->rs_nblocks;
	sampler->lt = InvalidOffsetNumber;
	sampler->estblocks = 2;
	sampler->doneblocks = 0;
	sampler->time = time;
	sampler->start_time = GetCurrentTimestamp();
	sampler->end_time = TimestampTzPlusMilliseconds(sampler->start_time,
													sampler->time);

	sampler_random_init_state(sampler->seed, sampler->randstate);

	/* Find relative prime as step size for linear probing. */
	sampler->step = random_relative_prime(sampler->nblocks, sampler->randstate);
	/*
	 * Randomize start position so that blocks close to step size don't have
	 * higher probability of being chosen on very short scan.
	 */
	sampler->lb = sampler_random_fract(sampler->randstate) * (sampler->nblocks / sampler->step);

	tsdesc->tsmdata = (void *) sampler;

	PG_RETURN_VOID();
}

/*
 * Get next block number or InvalidBlockNumber when we're done.
 *
 * Uses linear probing algorithm for picking next block.
 */
Datum
tsm_system_time_nextblock(PG_FUNCTION_ARGS)
{
	TableSampleDesc	   *tsdesc = (TableSampleDesc *) PG_GETARG_POINTER(0);
	SystemSamplerData  *sampler = (SystemSamplerData *) tsdesc->tsmdata;

	sampler->lb = (sampler->lb + sampler->step) % sampler->nblocks;
	sampler->doneblocks++;

	/* All blocks have been read, we're done */
	if (sampler->doneblocks > sampler->nblocks)
		PG_RETURN_UINT32(InvalidBlockNumber);

	/*
	 * Update the estimations for time limit at least 10 times per estimated
	 * number of returned blocks to handle variations in block read speed.
	 */
	if (sampler->doneblocks % Max(sampler->estblocks/10, 1) == 0)
	{
		TimestampTz	now = GetCurrentTimestamp();
		long        secs;
		int         usecs;
		int			usecs_remaining;
		int			time_per_block;

		TimestampDifference(sampler->start_time, now, &secs, &usecs);
		usecs += (int) secs * 1000000;

		time_per_block = usecs / sampler->doneblocks;

		/* No time left, end. */
		TimestampDifference(now, sampler->end_time, &secs, &usecs);
		if (secs <= 0 && usecs <= 0)
			PG_RETURN_UINT32(InvalidBlockNumber);

		/* Remaining microseconds */
		usecs_remaining = usecs + (int) secs * 1000000;

		/* Recalculate estimated returned number of blocks */
		if (time_per_block < usecs_remaining && time_per_block > 0)
			sampler->estblocks = sampler->time * time_per_block;
	}

	PG_RETURN_UINT32(sampler->lb);
}

/*
 * Get next tuple offset in current block or InvalidOffsetNumber if we are done
 * with this block.
 */
Datum
tsm_system_time_nexttuple(PG_FUNCTION_ARGS)
{
	TableSampleDesc	   *tsdesc = (TableSampleDesc *) PG_GETARG_POINTER(0);
	OffsetNumber		maxoffset = PG_GETARG_UINT16(2);
	SystemSamplerData  *sampler = (SystemSamplerData *) tsdesc->tsmdata;
	OffsetNumber		tupoffset = sampler->lt;

	if (tupoffset == InvalidOffsetNumber)
		tupoffset = FirstOffsetNumber;
	else
		tupoffset++;

	if (tupoffset > maxoffset)
		tupoffset = InvalidOffsetNumber;

	sampler->lt = tupoffset;

	PG_RETURN_UINT16(tupoffset);
}

/*
 * Cleanup method.
 */
Datum
tsm_system_time_end(PG_FUNCTION_ARGS)
{
	TableSampleDesc *tsdesc = (TableSampleDesc *) PG_GETARG_POINTER(0);

	pfree(tsdesc->tsmdata);

	PG_RETURN_VOID();
}

/*
 * Reset state (called by ReScan).
 */
Datum
tsm_system_time_reset(PG_FUNCTION_ARGS)
{
	TableSampleDesc	   *tsdesc = (TableSampleDesc *) PG_GETARG_POINTER(0);
	SystemSamplerData  *sampler = (SystemSamplerData *) tsdesc->tsmdata;

	sampler->lt = InvalidOffsetNumber;
	sampler->start_time = GetCurrentTimestamp();
	sampler->end_time = TimestampTzPlusMilliseconds(sampler->start_time,
													sampler->time);
	sampler->estblocks = 2;
	sampler->doneblocks = 0;

	sampler_random_init_state(sampler->seed, sampler->randstate);
	sampler->step = random_relative_prime(sampler->nblocks, sampler->randstate);
	sampler->lb = sampler_random_fract(sampler->randstate) * (sampler->nblocks / sampler->step);

	PG_RETURN_VOID();
}

/*
 * Costing function.
 */
Datum
tsm_system_time_cost(PG_FUNCTION_ARGS)
{
	PlannerInfo	   *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Path		   *path = (Path *) PG_GETARG_POINTER(1);
	RelOptInfo	   *baserel = (RelOptInfo *) PG_GETARG_POINTER(2);
	List		   *args = (List *) PG_GETARG_POINTER(3);
	BlockNumber	   *pages = (BlockNumber *) PG_GETARG_POINTER(4);
	double		   *tuples = (double *) PG_GETARG_POINTER(5);
	Node		   *limitnode;
	int32			time;
	BlockNumber		relpages;
	double			reltuples;
	double			density;
	double			spc_random_page_cost;

	limitnode = linitial(args);
	limitnode = estimate_expression_value(root, limitnode);

	if (IsA(limitnode, RelabelType))
		limitnode = (Node *) ((RelabelType *) limitnode)->arg;

	if (IsA(limitnode, Const))
		time = DatumGetInt32(((Const *) limitnode)->constvalue);
	else
	{
		/* Default time (1s) if the estimation didn't return Const. */
		time = 1000;
	}

	relpages = baserel->pages;
	reltuples = baserel->tuples;

	/* estimate the tuple density */
	if (relpages > 0)
		density = reltuples / (double) relpages;
	else
		density = (BLCKSZ - SizeOfPageHeaderData) / baserel->width;

	/*
	 * We equal random page cost value to number of ms it takes to read the
	 * random page here which is far from accurate but we don't have anything
	 * better to base our predicted page reads.
	 */
	get_tablespace_page_costs(baserel->reltablespace,
							  &spc_random_page_cost,
							  NULL);

	/*
	 * Assumption here is that we'll never read less then 1% of table pages,
	 * this is here mainly because it is much less bad to overestimate than
	 * underestimate and using just spc_random_page_cost will probably lead
	 * to underestimations in general.
	 */
	*pages = Min(baserel->pages, Max(time/spc_random_page_cost, baserel->pages/100));
	*tuples = rint(density * (double) *pages * path->rows / baserel->tuples);
	path->rows = *tuples;

	PG_RETURN_VOID();
}

static uint32
gcd (uint32 a, uint32 b)
{
	uint32 c;

	while (a != 0)
	{
		c = a;
		a = b % a;
		b = c;
	}

	return b;
}

static uint32
random_relative_prime(uint32 n, SamplerRandomState randstate)
{
	/* Pick random starting number, with some limits on what it can be. */
	uint32 r = (uint32) sampler_random_fract(randstate) * n/2 + n/4,
		   t;

	/*
	 * This should only take 2 or 3 iterations as the probability of 2 numbers
	 * being relatively prime is ~61%.
	 */
	while ((t = gcd(r, n)) > 1)
	{
		CHECK_FOR_INTERRUPTS();
		r /= t;
	}

	return r;
}
