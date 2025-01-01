/*-------------------------------------------------------------------------
 *
 * bernoulli.c
 *	  support routines for BERNOULLI tablesample method
 *
 * To ensure repeatability of samples, it is necessary that selection of a
 * given tuple be history-independent; otherwise syncscanning would break
 * repeatability, to say nothing of logically-irrelevant maintenance such
 * as physical extension or shortening of the relation.
 *
 * To achieve that, we proceed by hashing each candidate TID together with
 * the active seed, and then selecting it if the hash is less than the
 * cutoff value computed from the selection probability by BeginSampleScan.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/tablesample/bernoulli.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <math.h>

#include "access/tsmapi.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "optimizer/optimizer.h"
#include "utils/fmgrprotos.h"


/* Private state */
typedef struct
{
	uint64		cutoff;			/* select tuples with hash less than this */
	uint32		seed;			/* random seed */
	OffsetNumber lt;			/* last tuple returned from current block */
} BernoulliSamplerData;


static void bernoulli_samplescangetsamplesize(PlannerInfo *root,
											  RelOptInfo *baserel,
											  List *paramexprs,
											  BlockNumber *pages,
											  double *tuples);
static void bernoulli_initsamplescan(SampleScanState *node,
									 int eflags);
static void bernoulli_beginsamplescan(SampleScanState *node,
									  Datum *params,
									  int nparams,
									  uint32 seed);
static OffsetNumber bernoulli_nextsampletuple(SampleScanState *node,
											  BlockNumber blockno,
											  OffsetNumber maxoffset);


/*
 * Create a TsmRoutine descriptor for the BERNOULLI method.
 */
Datum
tsm_bernoulli_handler(PG_FUNCTION_ARGS)
{
	TsmRoutine *tsm = makeNode(TsmRoutine);

	tsm->parameterTypes = list_make1_oid(FLOAT4OID);
	tsm->repeatable_across_queries = true;
	tsm->repeatable_across_scans = true;
	tsm->SampleScanGetSampleSize = bernoulli_samplescangetsamplesize;
	tsm->InitSampleScan = bernoulli_initsamplescan;
	tsm->BeginSampleScan = bernoulli_beginsamplescan;
	tsm->NextSampleBlock = NULL;
	tsm->NextSampleTuple = bernoulli_nextsampletuple;
	tsm->EndSampleScan = NULL;

	PG_RETURN_POINTER(tsm);
}

/*
 * Sample size estimation.
 */
static void
bernoulli_samplescangetsamplesize(PlannerInfo *root,
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

	/* We'll visit all pages of the baserel */
	*pages = baserel->pages;

	*tuples = clamp_row_est(baserel->tuples * samplefract);
}

/*
 * Initialize during executor setup.
 */
static void
bernoulli_initsamplescan(SampleScanState *node, int eflags)
{
	node->tsm_state = palloc0(sizeof(BernoulliSamplerData));
}

/*
 * Examine parameters and prepare for a sample scan.
 */
static void
bernoulli_beginsamplescan(SampleScanState *node,
						  Datum *params,
						  int nparams,
						  uint32 seed)
{
	BernoulliSamplerData *sampler = (BernoulliSamplerData *) node->tsm_state;
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
	sampler->lt = InvalidOffsetNumber;

	/*
	 * Use bulkread, since we're scanning all pages.  But pagemode visibility
	 * checking is a win only at larger sampling fractions.  The 25% cutoff
	 * here is based on very limited experimentation.
	 */
	node->use_bulkread = true;
	node->use_pagemode = (percent >= 25);
}

/*
 * Select next sampled tuple in current block.
 *
 * It is OK here to return an offset without knowing if the tuple is visible
 * (or even exists).  The reason is that we do the coinflip for every tuple
 * offset in the table.  Since all tuples have the same probability of being
 * returned, it doesn't matter if we do extra coinflips for invisible tuples.
 *
 * When we reach end of the block, return InvalidOffsetNumber which tells
 * SampleScan to go to next block.
 */
static OffsetNumber
bernoulli_nextsampletuple(SampleScanState *node,
						  BlockNumber blockno,
						  OffsetNumber maxoffset)
{
	BernoulliSamplerData *sampler = (BernoulliSamplerData *) node->tsm_state;
	OffsetNumber tupoffset = sampler->lt;
	uint32		hashinput[3];

	/* Advance to first/next tuple in block */
	if (tupoffset == InvalidOffsetNumber)
		tupoffset = FirstOffsetNumber;
	else
		tupoffset++;

	/*
	 * We compute the hash by applying hash_any to an array of 3 uint32's
	 * containing the block, offset, and seed.  This is efficient to set up,
	 * and with the current implementation of hash_any, it gives
	 * machine-independent results, which is a nice property for regression
	 * testing.
	 *
	 * These words in the hash input are the same throughout the block:
	 */
	hashinput[0] = blockno;
	hashinput[2] = sampler->seed;

	/*
	 * Loop over tuple offsets until finding suitable TID or reaching end of
	 * block.
	 */
	for (; tupoffset <= maxoffset; tupoffset++)
	{
		uint32		hash;

		hashinput[1] = tupoffset;

		hash = DatumGetUInt32(hash_any((const unsigned char *) hashinput,
									   (int) sizeof(hashinput)));
		if (hash < sampler->cutoff)
			break;
	}

	if (tupoffset > maxoffset)
		tupoffset = InvalidOffsetNumber;

	sampler->lt = tupoffset;

	return tupoffset;
}
