/*-------------------------------------------------------------------------
 *
 * system.c
 *	  support routines for SYSTEM tablesample method
 *
 * To ensure repeatability of samples, it is necessary that selection of a
 * given tuple be history-independent; otherwise syncscanning would break
 * repeatability, to say nothing of logically-irrelevant maintenance such
 * as physical extension or shortening of the relation.
 *
 * To achieve that, we proceed by hashing each candidate block number together
 * with the active seed, and then selecting it if the hash is less than the
 * cutoff value computed from the selection probability by BeginSampleScan.
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/tablesample/system.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#ifdef _MSC_VER
#include <float.h>				/* for _isnan */
#endif
#include <math.h>

#include "access/hash.h"
#include "access/relscan.h"
#include "access/tsmapi.h"
#include "catalog/pg_type.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "utils/builtins.h"


/* Private state */
typedef struct
{
	uint64		cutoff;			/* select blocks with hash less than this */
	uint32		seed;			/* random seed */
	BlockNumber nextblock;		/* next block to consider sampling */
	OffsetNumber lt;			/* last tuple returned from current block */
} SystemSamplerData;


static void system_samplescangetsamplesize(PlannerInfo *root,
							   RelOptInfo *baserel,
							   List *paramexprs,
							   BlockNumber *pages,
							   double *tuples);
static void system_initsamplescan(SampleScanState *node,
					  int eflags);
static void system_beginsamplescan(SampleScanState *node,
					   Datum *params,
					   int nparams,
					   uint32 seed);
static BlockNumber system_nextsampleblock(SampleScanState *node);
static OffsetNumber system_nextsampletuple(SampleScanState *node,
					   BlockNumber blockno,
					   OffsetNumber maxoffset);


/*
 * Create a TsmRoutine descriptor for the SYSTEM method.
 */
Datum
tsm_system_handler(PG_FUNCTION_ARGS)
{
	TsmRoutine *tsm = makeNode(TsmRoutine);

	tsm->parameterTypes = list_make1_oid(FLOAT4OID);
	tsm->repeatable_across_queries = true;
	tsm->repeatable_across_scans = true;
	tsm->SampleScanGetSampleSize = system_samplescangetsamplesize;
	tsm->InitSampleScan = system_initsamplescan;
	tsm->BeginSampleScan = system_beginsamplescan;
	tsm->NextSampleBlock = system_nextsampleblock;
	tsm->NextSampleTuple = system_nextsampletuple;
	tsm->EndSampleScan = NULL;

	PG_RETURN_POINTER(tsm);
}

/*
 * Sample size estimation.
 */
static void
system_samplescangetsamplesize(PlannerInfo *root,
							   RelOptInfo *baserel,
							   List *paramexprs,
							   BlockNumber *pages,
							   double *tuples)
{
	Node	   *pctnode;
	float4		samplefract;

	/* Try to extract an estimate for the sample percentage */
	pctnode = (Node *) linitial(paramexprs);
	pctnode = estimate_expression_value(root, pctnode);

	if (IsA(pctnode, Const) &&
		!((Const *) pctnode)->constisnull)
	{
		samplefract = DatumGetFloat4(((Const *) pctnode)->constvalue);
		if (samplefract >= 0 && samplefract <= 100 && !isnan(samplefract))
			samplefract /= 100.0f;
		else
		{
			/* Default samplefract if the value is bogus */
			samplefract = 0.1f;
		}
	}
	else
	{
		/* Default samplefract if we didn't obtain a non-null Const */
		samplefract = 0.1f;
	}

	/* We'll visit a sample of the pages ... */
	*pages = clamp_row_est(baserel->pages * samplefract);

	/* ... and hopefully get a representative number of tuples from them */
	*tuples = clamp_row_est(baserel->tuples * samplefract);
}

/*
 * Initialize during executor setup.
 */
static void
system_initsamplescan(SampleScanState *node, int eflags)
{
	node->tsm_state = palloc0(sizeof(SystemSamplerData));
}

/*
 * Examine parameters and prepare for a sample scan.
 */
static void
system_beginsamplescan(SampleScanState *node,
					   Datum *params,
					   int nparams,
					   uint32 seed)
{
	SystemSamplerData *sampler = (SystemSamplerData *) node->tsm_state;
	double		percent = DatumGetFloat4(params[0]);
	double		dcutoff;

	if (percent < 0 || percent > 100 || isnan(percent))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLESAMPLE_ARGUMENT),
				 errmsg("sample percentage must be between 0 and 100")));

	/*
	 * The cutoff is sample probability times (PG_UINT32_MAX + 1); we have to
	 * store that as a uint64, of course.  Note that this gives strictly
	 * correct behavior at the limits of zero or one probability.
	 */
	dcutoff = rint(((double) PG_UINT32_MAX + 1) * percent / 100);
	sampler->cutoff = (uint64) dcutoff;
	sampler->seed = seed;
	sampler->nextblock = 0;
	sampler->lt = InvalidOffsetNumber;

	/*
	 * Bulkread buffer access strategy probably makes sense unless we're
	 * scanning a very small fraction of the table.  The 1% cutoff here is a
	 * guess.  We should use pagemode visibility checking, since we scan all
	 * tuples on each selected page.
	 */
	node->use_bulkread = (percent >= 1);
	node->use_pagemode = true;
}

/*
 * Select next block to sample.
 */
static BlockNumber
system_nextsampleblock(SampleScanState *node)
{
	SystemSamplerData *sampler = (SystemSamplerData *) node->tsm_state;
	HeapScanDesc scan = node->ss.ss_currentScanDesc;
	BlockNumber nextblock = sampler->nextblock;
	uint32		hashinput[2];

	/*
	 * We compute the hash by applying hash_any to an array of 2 uint32's
	 * containing the block number and seed.  This is efficient to set up, and
	 * with the current implementation of hash_any, it gives
	 * machine-independent results, which is a nice property for regression
	 * testing.
	 *
	 * These words in the hash input are the same throughout the block:
	 */
	hashinput[1] = sampler->seed;

	/*
	 * Loop over block numbers until finding suitable block or reaching end of
	 * relation.
	 */
	for (; nextblock < scan->rs_nblocks; nextblock++)
	{
		uint32		hash;

		hashinput[0] = nextblock;

		hash = DatumGetUInt32(hash_any((const unsigned char *) hashinput,
									   (int) sizeof(hashinput)));
		if (hash < sampler->cutoff)
			break;
	}

	if (nextblock < scan->rs_nblocks)
	{
		/* Found a suitable block; remember where we should start next time */
		sampler->nextblock = nextblock + 1;
		return nextblock;
	}

	/* Done, but let's reset nextblock to 0 for safety. */
	sampler->nextblock = 0;
	return InvalidBlockNumber;
}

/*
 * Select next sampled tuple in current block.
 *
 * In block sampling, we just want to sample all the tuples in each selected
 * block.
 *
 * It is OK here to return an offset without knowing if the tuple is visible
 * (or even exists); nodeSamplescan.c will deal with that.
 *
 * When we reach end of the block, return InvalidOffsetNumber which tells
 * SampleScan to go to next block.
 */
static OffsetNumber
system_nextsampletuple(SampleScanState *node,
					   BlockNumber blockno,
					   OffsetNumber maxoffset)
{
	SystemSamplerData *sampler = (SystemSamplerData *) node->tsm_state;
	OffsetNumber tupoffset = sampler->lt;

	/* Advance to next possible offset on page */
	if (tupoffset == InvalidOffsetNumber)
		tupoffset = FirstOffsetNumber;
	else
		tupoffset++;

	/* Done? */
	if (tupoffset > maxoffset)
		tupoffset = InvalidOffsetNumber;

	sampler->lt = tupoffset;

	return tupoffset;
}
