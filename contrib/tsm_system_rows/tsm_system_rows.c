/*-------------------------------------------------------------------------
 *
 * tsm_system_rows.c
 *	  interface routines for system_rows tablesample method
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/tsm_system_rows_rowlimit/tsm_system_rows.c
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

PG_MODULE_MAGIC;

/*
 * State
 */
typedef struct
{
	SamplerRandomState randstate;
	uint32			seed;			/* random seed */
	BlockNumber		nblocks;		/* number of block in relation */
	int32			ntuples;		/* number of tuples to return */
	int32			donetuples;		/* tuples already returned */
	OffsetNumber	lt;				/* last tuple returned from current block */
	BlockNumber		step;			/* step size */
	BlockNumber		lb;				/* last block visited */
	BlockNumber		doneblocks;		/* number of already returned blocks */
} SystemSamplerData;


PG_FUNCTION_INFO_V1(tsm_system_rows_init);
PG_FUNCTION_INFO_V1(tsm_system_rows_nextblock);
PG_FUNCTION_INFO_V1(tsm_system_rows_nexttuple);
PG_FUNCTION_INFO_V1(tsm_system_rows_examinetuple);
PG_FUNCTION_INFO_V1(tsm_system_rows_end);
PG_FUNCTION_INFO_V1(tsm_system_rows_reset);
PG_FUNCTION_INFO_V1(tsm_system_rows_cost);

static uint32 random_relative_prime(uint32 n, SamplerRandomState randstate);

/*
 * Initializes the state.
 */
Datum
tsm_system_rows_init(PG_FUNCTION_ARGS)
{
	TableSampleDesc	   *tsdesc = (TableSampleDesc *) PG_GETARG_POINTER(0);
	uint32				seed = PG_GETARG_UINT32(1);
	int32				ntuples = PG_ARGISNULL(2) ? -1 : PG_GETARG_INT32(2);
	HeapScanDesc		scan = tsdesc->heapScan;
	SystemSamplerData  *sampler;

	if (ntuples < 1)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("invalid sample size"),
				 errhint("Sample size must be positive integer value.")));

	sampler = palloc0(sizeof(SystemSamplerData));

	/* Remember initial values for reinit */
	sampler->seed = seed;
	sampler->nblocks = scan->rs_nblocks;
	sampler->ntuples = ntuples;
	sampler->donetuples = 0;
	sampler->lt = InvalidOffsetNumber;
	sampler->doneblocks = 0;

	sampler_random_init_state(sampler->seed, sampler->randstate);

	/* Find relative prime as step size for linear probing. */
	sampler->step = random_relative_prime(sampler->nblocks, sampler->randstate);
	/*
	 * Randomize start position so that blocks close to step size don't have
	 * higher probability of being chosen on very short scan.
	 */
	sampler->lb = sampler_random_fract(sampler->randstate) *
		(sampler->nblocks / sampler->step);

	tsdesc->tsmdata = (void *) sampler;

	PG_RETURN_VOID();
}

/*
 * Get next block number or InvalidBlockNumber when we're done.
 *
 * Uses linear probing algorithm for picking next block.
 */
Datum
tsm_system_rows_nextblock(PG_FUNCTION_ARGS)
{
	TableSampleDesc	   *tsdesc = (TableSampleDesc *) PG_GETARG_POINTER(0);
	SystemSamplerData  *sampler = (SystemSamplerData *) tsdesc->tsmdata;

	sampler->lb = (sampler->lb + sampler->step) % sampler->nblocks;
	sampler->doneblocks++;

	/* All blocks have been read, we're done */
	if (sampler->doneblocks > sampler->nblocks ||
		sampler->donetuples >= sampler->ntuples)
		PG_RETURN_UINT32(InvalidBlockNumber);

	PG_RETURN_UINT32(sampler->lb);
}

/*
 * Get next tuple offset in current block or InvalidOffsetNumber if we are done
 * with this block.
 */
Datum
tsm_system_rows_nexttuple(PG_FUNCTION_ARGS)
{
	TableSampleDesc	   *tsdesc = (TableSampleDesc *) PG_GETARG_POINTER(0);
	OffsetNumber		maxoffset = PG_GETARG_UINT16(2);
	SystemSamplerData  *sampler = (SystemSamplerData *) tsdesc->tsmdata;
	OffsetNumber		tupoffset = sampler->lt;

	if (tupoffset == InvalidOffsetNumber)
		tupoffset = FirstOffsetNumber;
	else
		tupoffset++;

	if (tupoffset > maxoffset ||
		sampler->donetuples >= sampler->ntuples)
		tupoffset = InvalidOffsetNumber;

	sampler->lt = tupoffset;

	PG_RETURN_UINT16(tupoffset);
}

/*
 * Examine tuple and decide if it should be returned.
 */
Datum
tsm_system_rows_examinetuple(PG_FUNCTION_ARGS)
{
	TableSampleDesc	   *tsdesc = (TableSampleDesc *) PG_GETARG_POINTER(0);
	bool				visible = PG_GETARG_BOOL(3);
	SystemSamplerData  *sampler = (SystemSamplerData *) tsdesc->tsmdata;

	if (!visible)
		PG_RETURN_BOOL(false);

	sampler->donetuples++;

	PG_RETURN_BOOL(true);
}

/*
 * Cleanup method.
 */
Datum
tsm_system_rows_end(PG_FUNCTION_ARGS)
{
	TableSampleDesc *tsdesc = (TableSampleDesc *) PG_GETARG_POINTER(0);

	pfree(tsdesc->tsmdata);

	PG_RETURN_VOID();
}

/*
 * Reset state (called by ReScan).
 */
Datum
tsm_system_rows_reset(PG_FUNCTION_ARGS)
{
	TableSampleDesc	   *tsdesc = (TableSampleDesc *) PG_GETARG_POINTER(0);
	SystemSamplerData  *sampler = (SystemSamplerData *) tsdesc->tsmdata;

	sampler->lt = InvalidOffsetNumber;
	sampler->donetuples = 0;
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
tsm_system_rows_cost(PG_FUNCTION_ARGS)
{
	PlannerInfo	   *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Path		   *path = (Path *) PG_GETARG_POINTER(1);
	RelOptInfo	   *baserel = (RelOptInfo *) PG_GETARG_POINTER(2);
	List		   *args = (List *) PG_GETARG_POINTER(3);
	BlockNumber	   *pages = (BlockNumber *) PG_GETARG_POINTER(4);
	double		   *tuples = (double *) PG_GETARG_POINTER(5);
	Node		   *limitnode;
	int32			ntuples;

	limitnode = linitial(args);
	limitnode = estimate_expression_value(root, limitnode);

	if (IsA(limitnode, RelabelType))
		limitnode = (Node *) ((RelabelType *) limitnode)->arg;

	if (IsA(limitnode, Const))
		ntuples = DatumGetInt32(((Const *) limitnode)->constvalue);
	else
	{
		/* Default ntuples if the estimation didn't return Const. */
		ntuples = 1000;
	}

	*pages = Min(baserel->pages, ntuples);
	*tuples = ntuples;
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
