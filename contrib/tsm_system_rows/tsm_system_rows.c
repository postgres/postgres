/*-------------------------------------------------------------------------
 *
 * tsm_system_rows.c
 *	  support routines for SYSTEM_ROWS tablesample method
 *
 * The desire here is to produce a random sample with a given number of rows
 * (or the whole relation, if that is fewer rows).  We use a block-sampling
 * approach.  To ensure that the whole relation will be visited if necessary,
 * we start at a randomly chosen block and then advance with a stride that
 * is randomly chosen but is relatively prime to the relation's nblocks.
 *
 * Because of the dependence on nblocks, this method cannot be repeatable
 * across queries.  (Even if the user hasn't explicitly changed the relation,
 * maintenance activities such as autovacuum might change nblocks.)  However,
 * we can at least make it repeatable across scans, by determining the
 * sampling pattern only once on the first scan.  This means that rescans
 * won't visit blocks added after the first scan, but that is fine since
 * such blocks shouldn't contain any visible tuples anyway.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  contrib/tsm_system_rows/tsm_system_rows.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relscan.h"
#include "access/tsmapi.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "utils/sampling.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(tsm_system_rows_handler);


/* Private state */
typedef struct
{
	uint32		seed;			/* random seed */
	int64		ntuples;		/* number of tuples to return */
	int64		donetuples;		/* number of tuples already returned */
	OffsetNumber lt;			/* last tuple returned from current block */
	BlockNumber doneblocks;		/* number of already-scanned blocks */
	BlockNumber lb;				/* last block visited */
	/* these three values are not changed during a rescan: */
	BlockNumber nblocks;		/* number of blocks in relation */
	BlockNumber firstblock;		/* first block to sample from */
	BlockNumber step;			/* step size, or 0 if not set yet */
} SystemRowsSamplerData;

static void system_rows_samplescangetsamplesize(PlannerInfo *root,
									RelOptInfo *baserel,
									List *paramexprs,
									BlockNumber *pages,
									double *tuples);
static void system_rows_initsamplescan(SampleScanState *node,
						   int eflags);
static void system_rows_beginsamplescan(SampleScanState *node,
							Datum *params,
							int nparams,
							uint32 seed);
static BlockNumber system_rows_nextsampleblock(SampleScanState *node);
static OffsetNumber system_rows_nextsampletuple(SampleScanState *node,
							BlockNumber blockno,
							OffsetNumber maxoffset);
static bool SampleOffsetVisible(OffsetNumber tupoffset, HeapScanDesc scan);
static uint32 random_relative_prime(uint32 n, SamplerRandomState randstate);


/*
 * Create a TsmRoutine descriptor for the SYSTEM_ROWS method.
 */
Datum
tsm_system_rows_handler(PG_FUNCTION_ARGS)
{
	TsmRoutine *tsm = makeNode(TsmRoutine);

	tsm->parameterTypes = list_make1_oid(INT8OID);

	/* See notes at head of file */
	tsm->repeatable_across_queries = false;
	tsm->repeatable_across_scans = true;

	tsm->SampleScanGetSampleSize = system_rows_samplescangetsamplesize;
	tsm->InitSampleScan = system_rows_initsamplescan;
	tsm->BeginSampleScan = system_rows_beginsamplescan;
	tsm->NextSampleBlock = system_rows_nextsampleblock;
	tsm->NextSampleTuple = system_rows_nextsampletuple;
	tsm->EndSampleScan = NULL;

	PG_RETURN_POINTER(tsm);
}

/*
 * Sample size estimation.
 */
static void
system_rows_samplescangetsamplesize(PlannerInfo *root,
									RelOptInfo *baserel,
									List *paramexprs,
									BlockNumber *pages,
									double *tuples)
{
	Node	   *limitnode;
	int64		ntuples;
	double		npages;

	/* Try to extract an estimate for the limit rowcount */
	limitnode = (Node *) linitial(paramexprs);
	limitnode = estimate_expression_value(root, limitnode);

	if (IsA(limitnode, Const) &&
		!((Const *) limitnode)->constisnull)
	{
		ntuples = DatumGetInt64(((Const *) limitnode)->constvalue);
		if (ntuples < 0)
		{
			/* Default ntuples if the value is bogus */
			ntuples = 1000;
		}
	}
	else
	{
		/* Default ntuples if we didn't obtain a non-null Const */
		ntuples = 1000;
	}

	/* Clamp to the estimated relation size */
	if (ntuples > baserel->tuples)
		ntuples = (int64) baserel->tuples;
	ntuples = clamp_row_est(ntuples);

	if (baserel->tuples > 0 && baserel->pages > 0)
	{
		/* Estimate number of pages visited based on tuple density */
		double		density = baserel->tuples / (double) baserel->pages;

		npages = ntuples / density;
	}
	else
	{
		/* For lack of data, assume one tuple per page */
		npages = ntuples;
	}

	/* Clamp to sane value */
	npages = clamp_row_est(Min((double) baserel->pages, npages));

	*pages = npages;
	*tuples = ntuples;
}

/*
 * Initialize during executor setup.
 */
static void
system_rows_initsamplescan(SampleScanState *node, int eflags)
{
	node->tsm_state = palloc0(sizeof(SystemRowsSamplerData));
	/* Note the above leaves tsm_state->step equal to zero */
}

/*
 * Examine parameters and prepare for a sample scan.
 */
static void
system_rows_beginsamplescan(SampleScanState *node,
							Datum *params,
							int nparams,
							uint32 seed)
{
	SystemRowsSamplerData *sampler = (SystemRowsSamplerData *) node->tsm_state;
	int64		ntuples = DatumGetInt64(params[0]);

	if (ntuples < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLESAMPLE_ARGUMENT),
				 errmsg("sample size must not be negative")));

	sampler->seed = seed;
	sampler->ntuples = ntuples;
	sampler->donetuples = 0;
	sampler->lt = InvalidOffsetNumber;
	sampler->doneblocks = 0;
	/* lb will be initialized during first NextSampleBlock call */
	/* we intentionally do not change nblocks/firstblock/step here */

	/*
	 * We *must* use pagemode visibility checking in this module, so force
	 * that even though it's currently default.
	 */
	node->use_pagemode = true;
}

/*
 * Select next block to sample.
 *
 * Uses linear probing algorithm for picking next block.
 */
static BlockNumber
system_rows_nextsampleblock(SampleScanState *node)
{
	SystemRowsSamplerData *sampler = (SystemRowsSamplerData *) node->tsm_state;
	HeapScanDesc scan = node->ss.ss_currentScanDesc;

	/* First call within scan? */
	if (sampler->doneblocks == 0)
	{
		/* First scan within query? */
		if (sampler->step == 0)
		{
			/* Initialize now that we have scan descriptor */
			SamplerRandomState randstate;

			/* If relation is empty, there's nothing to scan */
			if (scan->rs_nblocks == 0)
				return InvalidBlockNumber;

			/* We only need an RNG during this setup step */
			sampler_random_init_state(sampler->seed, randstate);

			/* Compute nblocks/firstblock/step only once per query */
			sampler->nblocks = scan->rs_nblocks;

			/* Choose random starting block within the relation */
			/* (Actually this is the predecessor of the first block visited) */
			sampler->firstblock = sampler_random_fract(randstate) *
				sampler->nblocks;

			/* Find relative prime as step size for linear probing */
			sampler->step = random_relative_prime(sampler->nblocks, randstate);
		}

		/* Reinitialize lb */
		sampler->lb = sampler->firstblock;
	}

	/* If we've read all blocks or returned all needed tuples, we're done */
	if (++sampler->doneblocks > sampler->nblocks ||
		sampler->donetuples >= sampler->ntuples)
		return InvalidBlockNumber;

	/*
	 * It's probably impossible for scan->rs_nblocks to decrease between scans
	 * within a query; but just in case, loop until we select a block number
	 * less than scan->rs_nblocks.  We don't care if scan->rs_nblocks has
	 * increased since the first scan.
	 */
	do
	{
		/* Advance lb, using uint64 arithmetic to forestall overflow */
		sampler->lb = ((uint64) sampler->lb + sampler->step) % sampler->nblocks;
	} while (sampler->lb >= scan->rs_nblocks);

	return sampler->lb;
}

/*
 * Select next sampled tuple in current block.
 *
 * In block sampling, we just want to sample all the tuples in each selected
 * block.
 *
 * When we reach end of the block, return InvalidOffsetNumber which tells
 * SampleScan to go to next block.
 */
static OffsetNumber
system_rows_nextsampletuple(SampleScanState *node,
							BlockNumber blockno,
							OffsetNumber maxoffset)
{
	SystemRowsSamplerData *sampler = (SystemRowsSamplerData *) node->tsm_state;
	HeapScanDesc scan = node->ss.ss_currentScanDesc;
	OffsetNumber tupoffset = sampler->lt;

	/* Quit if we've returned all needed tuples */
	if (sampler->donetuples >= sampler->ntuples)
		return InvalidOffsetNumber;

	/*
	 * Because we should only count visible tuples as being returned, we need
	 * to search for a visible tuple rather than just let the core code do it.
	 */

	/* We rely on the data accumulated in pagemode access */
	Assert(scan->rs_pageatatime);
	for (;;)
	{
		/* Advance to next possible offset on page */
		if (tupoffset == InvalidOffsetNumber)
			tupoffset = FirstOffsetNumber;
		else
			tupoffset++;

		/* Done? */
		if (tupoffset > maxoffset)
		{
			tupoffset = InvalidOffsetNumber;
			break;
		}

		/* Found a candidate? */
		if (SampleOffsetVisible(tupoffset, scan))
		{
			sampler->donetuples++;
			break;
		}
	}

	sampler->lt = tupoffset;

	return tupoffset;
}

/*
 * Check if tuple offset is visible
 *
 * In pageatatime mode, heapgetpage() already did visibility checks,
 * so just look at the info it left in rs_vistuples[].
 */
static bool
SampleOffsetVisible(OffsetNumber tupoffset, HeapScanDesc scan)
{
	int			start = 0,
				end = scan->rs_ntuples - 1;

	while (start <= end)
	{
		int			mid = (start + end) / 2;
		OffsetNumber curoffset = scan->rs_vistuples[mid];

		if (tupoffset == curoffset)
			return true;
		else if (tupoffset < curoffset)
			end = mid - 1;
		else
			start = mid + 1;
	}

	return false;
}

/*
 * Compute greatest common divisor of two uint32's.
 */
static uint32
gcd(uint32 a, uint32 b)
{
	uint32		c;

	while (a != 0)
	{
		c = a;
		a = b % a;
		b = c;
	}

	return b;
}

/*
 * Pick a random value less than and relatively prime to n, if possible
 * (else return 1).
 */
static uint32
random_relative_prime(uint32 n, SamplerRandomState randstate)
{
	uint32		r;

	/* Safety check to avoid infinite loop or zero result for small n. */
	if (n <= 1)
		return 1;

	/*
	 * This should only take 2 or 3 iterations as the probability of 2 numbers
	 * being relatively prime is ~61%; but just in case, we'll include a
	 * CHECK_FOR_INTERRUPTS in the loop.
	 */
	do
	{
		CHECK_FOR_INTERRUPTS();
		r = (uint32) (sampler_random_fract(randstate) * n);
	} while (r == 0 || gcd(r, n) > 1);

	return r;
}
