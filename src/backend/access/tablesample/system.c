/*-------------------------------------------------------------------------
 *
 * system.c
 *	  interface routines for system tablesample method
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/tablesample/system.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"

#include "access/tablesample.h"
#include "access/relscan.h"
#include "nodes/execnodes.h"
#include "nodes/relation.h"
#include "optimizer/clauses.h"
#include "storage/bufmgr.h"
#include "utils/sampling.h"


/*
 * State
 */
typedef struct
{
	BlockSamplerData bs;
	uint32 seed;				/* random seed */
	BlockNumber nblocks;		/* number of block in relation */
	int samplesize;				/* number of blocks to return */
	OffsetNumber lt;			/* last tuple returned from current block */
} SystemSamplerData;


/*
 * Initializes the state.
 */
Datum
tsm_system_init(PG_FUNCTION_ARGS)
{
	TableSampleDesc	   *tsdesc = (TableSampleDesc *) PG_GETARG_POINTER(0);
	uint32				seed = PG_GETARG_UINT32(1);
	float4				percent = PG_ARGISNULL(2) ? -1 : PG_GETARG_FLOAT4(2);
	HeapScanDesc		scan = tsdesc->heapScan;
	SystemSamplerData  *sampler;

	if (percent < 0 || percent > 100)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("invalid sample size"),
				 errhint("Sample size must be numeric value between 0 and 100 (inclusive).")));

	sampler = palloc0(sizeof(SystemSamplerData));

	/* Remember initial values for reinit */
	sampler->seed = seed;
	sampler->nblocks = scan->rs_nblocks;
	sampler->samplesize = 1 + (int) (sampler->nblocks * (percent / 100.0));
	sampler->lt = InvalidOffsetNumber;

	BlockSampler_Init(&sampler->bs, sampler->nblocks, sampler->samplesize,
					  sampler->seed);

	tsdesc->tsmdata = (void *) sampler;

	PG_RETURN_VOID();
}

/*
 * Get next block number or InvalidBlockNumber when we're done.
 *
 * Uses the same logic as ANALYZE for picking the random blocks.
 */
Datum
tsm_system_nextblock(PG_FUNCTION_ARGS)
{
	TableSampleDesc	   *tsdesc = (TableSampleDesc *) PG_GETARG_POINTER(0);
	SystemSamplerData  *sampler = (SystemSamplerData *) tsdesc->tsmdata;
	BlockNumber			blockno;

	if (!BlockSampler_HasMore(&sampler->bs))
		PG_RETURN_UINT32(InvalidBlockNumber);

	blockno = BlockSampler_Next(&sampler->bs);

	PG_RETURN_UINT32(blockno);
}

/*
 * Get next tuple offset in current block or InvalidOffsetNumber if we are done
 * with this block.
 */
Datum
tsm_system_nexttuple(PG_FUNCTION_ARGS)
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
tsm_system_end(PG_FUNCTION_ARGS)
{
	TableSampleDesc *tsdesc = (TableSampleDesc *) PG_GETARG_POINTER(0);

	pfree(tsdesc->tsmdata);

	PG_RETURN_VOID();
}

/*
 * Reset state (called by ReScan).
 */
Datum
tsm_system_reset(PG_FUNCTION_ARGS)
{
	TableSampleDesc	   *tsdesc = (TableSampleDesc *) PG_GETARG_POINTER(0);
	SystemSamplerData  *sampler = (SystemSamplerData *) tsdesc->tsmdata;

	sampler->lt = InvalidOffsetNumber;
	BlockSampler_Init(&sampler->bs, sampler->nblocks, sampler->samplesize,
					  sampler->seed);

	PG_RETURN_VOID();
}

/*
 * Costing function.
 */
Datum
tsm_system_cost(PG_FUNCTION_ARGS)
{
	PlannerInfo	   *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Path		   *path = (Path *) PG_GETARG_POINTER(1);
	RelOptInfo	   *baserel = (RelOptInfo *) PG_GETARG_POINTER(2);
	List		   *args = (List *) PG_GETARG_POINTER(3);
	BlockNumber	   *pages = (BlockNumber *) PG_GETARG_POINTER(4);
	double		   *tuples = (double *) PG_GETARG_POINTER(5);
	Node		   *pctnode;
	float4			samplesize;

	pctnode = linitial(args);
	pctnode = estimate_expression_value(root, pctnode);

	if (IsA(pctnode, RelabelType))
		pctnode = (Node *) ((RelabelType *) pctnode)->arg;

	if (IsA(pctnode, Const))
	{
		samplesize = DatumGetFloat4(((Const *) pctnode)->constvalue);
		samplesize /= 100.0;
	}
	else
	{
		/* Default samplesize if the estimation didn't return Const. */
		samplesize = 0.1f;
	}

	*pages = baserel->pages * samplesize;
	*tuples = path->rows * samplesize;
	path->rows = *tuples;

	PG_RETURN_VOID();
}
