/*-------------------------------------------------------------------------
 *
 * nodeHash.c
 *	  Routines to hash relations for hashjoin
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeHash.c,v 1.79 2003/08/04 02:39:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecHash		- generate an in-memory hash table of the relation
 *		ExecInitHash	- initialize node and subnodes
 *		ExecEndHash		- shutdown node and subnodes
 */
#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "miscadmin.h"
#include "parser/parse_expr.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"


/* ----------------------------------------------------------------
 *		ExecHash
 *
 *		build hash table for hashjoin, all do partitioning if more
 *		than one batches are required.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecHash(HashState *node)
{
	EState	   *estate;
	PlanState  *outerNode;
	List	   *hashkeys;
	HashJoinTable hashtable;
	TupleTableSlot *slot;
	ExprContext *econtext;
	int			nbatch;
	int			i;

	/*
	 * get state info from node
	 */
	estate = node->ps.state;
	outerNode = outerPlanState(node);

	hashtable = node->hashtable;
	nbatch = hashtable->nbatch;

	if (nbatch > 0)
	{
		/*
		 * Open temp files for inner batches, if needed. Note that file
		 * buffers are palloc'd in regular executor context.
		 */
		for (i = 0; i < nbatch; i++)
			hashtable->innerBatchFile[i] = BufFileCreateTemp(false);
	}

	/*
	 * set expression context
	 */
	hashkeys = node->hashkeys;
	econtext = node->ps.ps_ExprContext;

	/*
	 * get all inner tuples and insert into the hash table (or temp files)
	 */
	for (;;)
	{
		slot = ExecProcNode(outerNode);
		if (TupIsNull(slot))
			break;
		econtext->ecxt_innertuple = slot;
		ExecHashTableInsert(hashtable, econtext, hashkeys);
		ExecClearTuple(slot);
	}

	/*
	 * Return the slot so that we have the tuple descriptor when we need
	 * to save/restore them.  -Jeff 11 July 1991
	 */
	return slot;
}

/* ----------------------------------------------------------------
 *		ExecInitHash
 *
 *		Init routine for Hash node
 * ----------------------------------------------------------------
 */
HashState *
ExecInitHash(Hash *node, EState *estate)
{
	HashState  *hashstate;

	SO_printf("ExecInitHash: initializing hash node\n");

	/*
	 * create state structure
	 */
	hashstate = makeNode(HashState);
	hashstate->ps.plan = (Plan *) node;
	hashstate->ps.state = estate;
	hashstate->hashtable = NULL;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &hashstate->ps);

#define HASH_NSLOTS 1

	/*
	 * initialize our result slot
	 */
	ExecInitResultTupleSlot(estate, &hashstate->ps);

	/*
	 * initialize child expressions
	 */
	hashstate->ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->plan.targetlist,
					 (PlanState *) hashstate);
	hashstate->ps.qual = (List *)
		ExecInitExpr((Expr *) node->plan.qual,
					 (PlanState *) hashstate);

	/*
	 * initialize child nodes
	 */
	outerPlanState(hashstate) = ExecInitNode(outerPlan(node), estate);

	/*
	 * initialize tuple type. no need to initialize projection info
	 * because this node doesn't do projections
	 */
	ExecAssignResultTypeFromOuterPlan(&hashstate->ps);
	hashstate->ps.ps_ProjInfo = NULL;

	return hashstate;
}

int
ExecCountSlotsHash(Hash *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
		ExecCountSlotsNode(innerPlan(node)) +
		HASH_NSLOTS;
}

/* ---------------------------------------------------------------
 *		ExecEndHash
 *
 *		clean up routine for Hash node
 * ----------------------------------------------------------------
 */
void
ExecEndHash(HashState *node)
{
	PlanState  *outerPlan;

	/*
	 * free exprcontext
	 */
	ExecFreeExprContext(&node->ps);

	/*
	 * shut down the subplan
	 */
	outerPlan = outerPlanState(node);
	ExecEndNode(outerPlan);
}


/* ----------------------------------------------------------------
 *		ExecHashTableCreate
 *
 *		create a hashtable in shared memory for hashjoin.
 * ----------------------------------------------------------------
 */
HashJoinTable
ExecHashTableCreate(Hash *node, List *hashOperators)
{
	HashJoinTable hashtable;
	Plan	   *outerNode;
	int			totalbuckets;
	int			nbuckets;
	int			nbatch;
	int			nkeys;
	int			i;
	List	   *ho;
	MemoryContext oldcxt;

	/*
	 * Get information about the size of the relation to be hashed (it's
	 * the "outer" subtree of this node, but the inner relation of the
	 * hashjoin).  Compute the appropriate size of the hash table.
	 */
	outerNode = outerPlan(node);

	ExecChooseHashTableSize(outerNode->plan_rows, outerNode->plan_width,
							&totalbuckets, &nbuckets, &nbatch);

#ifdef HJDEBUG
	printf("nbatch = %d, totalbuckets = %d, nbuckets = %d\n",
		   nbatch, totalbuckets, nbuckets);
#endif

	/*
	 * Initialize the hash table control block.
	 *
	 * The hashtable control block is just palloc'd from the executor's
	 * per-query memory context.
	 */
	hashtable = (HashJoinTable) palloc(sizeof(HashTableData));
	hashtable->nbuckets = nbuckets;
	hashtable->totalbuckets = totalbuckets;
	hashtable->buckets = NULL;
	hashtable->nbatch = nbatch;
	hashtable->curbatch = 0;
	hashtable->innerBatchFile = NULL;
	hashtable->outerBatchFile = NULL;
	hashtable->innerBatchSize = NULL;
	hashtable->outerBatchSize = NULL;

	/*
	 * Get info about the hash functions to be used for each hash key.
	 */
	nkeys = length(hashOperators);
	hashtable->hashfunctions = (FmgrInfo *) palloc(nkeys * sizeof(FmgrInfo));
	i = 0;
	foreach(ho, hashOperators)
	{
		Oid			hashfn;

		hashfn = get_op_hash_function(lfirsto(ho));
		if (!OidIsValid(hashfn))
			elog(ERROR, "could not find hash function for hash operator %u",
				 lfirsto(ho));
		fmgr_info(hashfn, &hashtable->hashfunctions[i]);
		i++;
	}

	/*
	 * Create temporary memory contexts in which to keep the hashtable
	 * working storage.  See notes in executor/hashjoin.h.
	 */
	hashtable->hashCxt = AllocSetContextCreate(CurrentMemoryContext,
											   "HashTableContext",
											   ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);

	hashtable->batchCxt = AllocSetContextCreate(hashtable->hashCxt,
												"HashBatchContext",
												ALLOCSET_DEFAULT_MINSIZE,
												ALLOCSET_DEFAULT_INITSIZE,
												ALLOCSET_DEFAULT_MAXSIZE);

	/* Allocate data that will live for the life of the hashjoin */

	oldcxt = MemoryContextSwitchTo(hashtable->hashCxt);

	if (nbatch > 0)
	{
		/*
		 * allocate and initialize the file arrays in hashCxt
		 */
		hashtable->innerBatchFile = (BufFile **)
			palloc0(nbatch * sizeof(BufFile *));
		hashtable->outerBatchFile = (BufFile **)
			palloc0(nbatch * sizeof(BufFile *));
		hashtable->innerBatchSize = (long *)
			palloc0(nbatch * sizeof(long));
		hashtable->outerBatchSize = (long *)
			palloc0(nbatch * sizeof(long));
		/* The files will not be opened until later... */
	}

	/*
	 * Prepare context for the first-scan space allocations; allocate the
	 * hashbucket array therein, and set each bucket "empty".
	 */
	MemoryContextSwitchTo(hashtable->batchCxt);

	hashtable->buckets = (HashJoinTuple *)
		palloc0(nbuckets * sizeof(HashJoinTuple));

	MemoryContextSwitchTo(oldcxt);

	return hashtable;
}


/*
 * Compute appropriate size for hashtable given the estimated size of the
 * relation to be hashed (number of rows and average row width).
 *
 * Caution: the input is only the planner's estimates, and so can't be
 * trusted too far.  Apply a healthy fudge factor.
 *
 * This is exported so that the planner's costsize.c can use it.
 */

/* Target bucket loading (tuples per bucket) */
#define NTUP_PER_BUCKET			10
/* Fudge factor to allow for inaccuracy of input estimates */
#define FUDGE_FAC				2.0

void
ExecChooseHashTableSize(double ntuples, int tupwidth,
						int *virtualbuckets,
						int *physicalbuckets,
						int *numbatches)
{
	int			tupsize;
	double		inner_rel_bytes;
	long		hash_table_bytes;
	double		dtmp;
	int			nbatch;
	int			nbuckets;
	int			totalbuckets;
	int			bucketsize;

	/* Force a plausible relation size if no info */
	if (ntuples <= 0.0)
		ntuples = 1000.0;

	/*
	 * Estimate tupsize based on footprint of tuple in hashtable... but
	 * what about palloc overhead?
	 */
	tupsize = MAXALIGN(tupwidth) + MAXALIGN(sizeof(HashJoinTupleData));
	inner_rel_bytes = ntuples * tupsize * FUDGE_FAC;

	/*
	 * Target in-memory hashtable size is SortMem kilobytes.
	 */
	hash_table_bytes = SortMem * 1024L;

	/*
	 * Count the number of hash buckets we want for the whole relation,
	 * for an average bucket load of NTUP_PER_BUCKET (per virtual
	 * bucket!).  It has to fit in an int, however.
	 */
	dtmp = ceil(ntuples * FUDGE_FAC / NTUP_PER_BUCKET);
	if (dtmp < INT_MAX)
		totalbuckets = (int) dtmp;
	else
		totalbuckets = INT_MAX;
	if (totalbuckets <= 0)
		totalbuckets = 1;

	/*
	 * Count the number of buckets we think will actually fit in the
	 * target memory size, at a loading of NTUP_PER_BUCKET (physical
	 * buckets). NOTE: FUDGE_FAC here determines the fraction of the
	 * hashtable space reserved to allow for nonuniform distribution of
	 * hash values. Perhaps this should be a different number from the
	 * other uses of FUDGE_FAC, but since we have no real good way to pick
	 * either one...
	 */
	bucketsize = NTUP_PER_BUCKET * tupsize;
	nbuckets = (int) (hash_table_bytes / (bucketsize * FUDGE_FAC));
	if (nbuckets <= 0)
		nbuckets = 1;

	if (totalbuckets <= nbuckets)
	{
		/*
		 * We have enough space, so no batching.  In theory we could even
		 * reduce nbuckets, but since that could lead to poor behavior if
		 * estimated ntuples is much less than reality, it seems better to
		 * make more buckets instead of fewer.
		 */
		totalbuckets = nbuckets;
		nbatch = 0;
	}
	else
	{
		/*
		 * Need to batch; compute how many batches we want to use. Note
		 * that nbatch doesn't have to have anything to do with the ratio
		 * totalbuckets/nbuckets; in fact, it is the number of groups we
		 * will use for the part of the data that doesn't fall into the
		 * first nbuckets hash buckets.  We try to set it to make all the
		 * batches the same size.
		 */
		dtmp = ceil((inner_rel_bytes - hash_table_bytes) /
					hash_table_bytes);
		if (dtmp < INT_MAX)
			nbatch = (int) dtmp;
		else
			nbatch = INT_MAX;
		if (nbatch <= 0)
			nbatch = 1;
	}

	/*
	 * Now, totalbuckets is the number of (virtual) hashbuckets for the
	 * whole relation, and nbuckets is the number of physical hashbuckets
	 * we will use in the first pass.  Data falling into the first
	 * nbuckets virtual hashbuckets gets handled in the first pass;
	 * everything else gets divided into nbatch batches to be processed in
	 * additional passes.
	 */
	*virtualbuckets = totalbuckets;
	*physicalbuckets = nbuckets;
	*numbatches = nbatch;
}


/* ----------------------------------------------------------------
 *		ExecHashTableDestroy
 *
 *		destroy a hash table
 * ----------------------------------------------------------------
 */
void
ExecHashTableDestroy(HashJoinTable hashtable)
{
	int			i;

	/* Make sure all the temp files are closed */
	for (i = 0; i < hashtable->nbatch; i++)
	{
		if (hashtable->innerBatchFile[i])
			BufFileClose(hashtable->innerBatchFile[i]);
		if (hashtable->outerBatchFile[i])
			BufFileClose(hashtable->outerBatchFile[i]);
	}

	/* Release working memory (batchCxt is a child, so it goes away too) */
	MemoryContextDelete(hashtable->hashCxt);

	/* And drop the control block */
	pfree(hashtable);
}

/* ----------------------------------------------------------------
 *		ExecHashTableInsert
 *
 *		insert a tuple into the hash table depending on the hash value
 *		it may just go to a tmp file for other batches
 * ----------------------------------------------------------------
 */
void
ExecHashTableInsert(HashJoinTable hashtable,
					ExprContext *econtext,
					List *hashkeys)
{
	int			bucketno = ExecHashGetBucket(hashtable, econtext, hashkeys);
	int			batchno = ExecHashGetBatch(bucketno, hashtable);
	TupleTableSlot *slot = econtext->ecxt_innertuple;
	HeapTuple	heapTuple = slot->val;

	/*
	 * decide whether to put the tuple in the hash table or a tmp file
	 */
	if (batchno < 0)
	{
		/*
		 * put the tuple in hash table
		 */
		HashJoinTuple hashTuple;
		int			hashTupleSize;

		hashTupleSize = MAXALIGN(sizeof(*hashTuple)) + heapTuple->t_len;
		hashTuple = (HashJoinTuple) MemoryContextAlloc(hashtable->batchCxt,
													   hashTupleSize);
		memcpy((char *) &hashTuple->htup,
			   (char *) heapTuple,
			   sizeof(hashTuple->htup));
		hashTuple->htup.t_datamcxt = hashtable->batchCxt;
		hashTuple->htup.t_data = (HeapTupleHeader)
			(((char *) hashTuple) + MAXALIGN(sizeof(*hashTuple)));
		memcpy((char *) hashTuple->htup.t_data,
			   (char *) heapTuple->t_data,
			   heapTuple->t_len);
		hashTuple->next = hashtable->buckets[bucketno];
		hashtable->buckets[bucketno] = hashTuple;
	}
	else
	{
		/*
		 * put the tuple into a tmp file for later batches
		 */
		hashtable->innerBatchSize[batchno]++;
		ExecHashJoinSaveTuple(heapTuple,
							  hashtable->innerBatchFile[batchno]);
	}
}

/* ----------------------------------------------------------------
 *		ExecHashGetBucket
 *
 *		Get the hash value for a tuple
 * ----------------------------------------------------------------
 */
int
ExecHashGetBucket(HashJoinTable hashtable,
				  ExprContext *econtext,
				  List *hashkeys)
{
	uint32		hashkey = 0;
	int			bucketno;
	List	   *hk;
	int			i = 0;
	MemoryContext oldContext;

	/*
	 * We reset the eval context each time to reclaim any memory leaked in
	 * the hashkey expressions.
	 */
	ResetExprContext(econtext);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	foreach(hk, hashkeys)
	{
		Datum		keyval;
		bool		isNull;

		/* rotate hashkey left 1 bit at each step */
		hashkey = (hashkey << 1) | ((hashkey & 0x80000000) ? 1 : 0);

		/*
		 * Get the join attribute value of the tuple
		 */
		keyval = ExecEvalExpr((ExprState *) lfirst(hk),
							  econtext, &isNull, NULL);

		/*
		 * Compute the hash function
		 */
		if (!isNull)			/* treat nulls as having hash key 0 */
		{
			uint32		hkey;

			hkey = DatumGetUInt32(FunctionCall1(&hashtable->hashfunctions[i],
												keyval));
			hashkey ^= hkey;
		}

		i++;
	}

	bucketno = hashkey % (uint32) hashtable->totalbuckets;

#ifdef HJDEBUG
	if (bucketno >= hashtable->nbuckets)
		printf("hash(%u) = %d SAVED\n", hashkey, bucketno);
	else
		printf("hash(%u) = %d\n", hashkey, bucketno);
#endif

	MemoryContextSwitchTo(oldContext);

	return bucketno;
}

/* ----------------------------------------------------------------
 *		ExecHashGetBatch
 *
 *		determine the batch number for a bucketno
 *
 * Returns -1 if bucket belongs to initial (or current) batch,
 * else 0..nbatch-1 corresponding to external batch file number for bucket.
 * ----------------------------------------------------------------
 */
int
ExecHashGetBatch(int bucketno, HashJoinTable hashtable)
{
	if (bucketno < hashtable->nbuckets)
		return -1;

	return (bucketno - hashtable->nbuckets) % hashtable->nbatch;
}

/* ----------------------------------------------------------------
 *		ExecScanHashBucket
 *
 *		scan a hash bucket of matches
 * ----------------------------------------------------------------
 */
HeapTuple
ExecScanHashBucket(HashJoinState *hjstate,
				   List *hjclauses,
				   ExprContext *econtext)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	HashJoinTuple hashTuple = hjstate->hj_CurTuple;

	/*
	 * hj_CurTuple is NULL to start scanning a new bucket, or the address
	 * of the last tuple returned from the current bucket.
	 */
	if (hashTuple == NULL)
		hashTuple = hashtable->buckets[hjstate->hj_CurBucketNo];
	else
		hashTuple = hashTuple->next;

	while (hashTuple != NULL)
	{
		HeapTuple	heapTuple = &hashTuple->htup;
		TupleTableSlot *inntuple;

		/* insert hashtable's tuple into exec slot so ExecQual sees it */
		inntuple = ExecStoreTuple(heapTuple,	/* tuple to store */
								  hjstate->hj_HashTupleSlot,	/* slot */
								  InvalidBuffer,
								  false);		/* do not pfree this tuple */
		econtext->ecxt_innertuple = inntuple;

		/* reset temp memory each time to avoid leaks from qual expression */
		ResetExprContext(econtext);

		if (ExecQual(hjclauses, econtext, false))
		{
			hjstate->hj_CurTuple = hashTuple;
			return heapTuple;
		}

		hashTuple = hashTuple->next;
	}

	/*
	 * no match
	 */
	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecHashTableReset
 *
 *		reset hash table header for new batch
 *
 *		ntuples is the number of tuples in the inner relation's batch
 *		(which we currently don't actually use...)
 * ----------------------------------------------------------------
 */
void
ExecHashTableReset(HashJoinTable hashtable, long ntuples)
{
	MemoryContext oldcxt;
	int			nbuckets = hashtable->nbuckets;

	/*
	 * Release all the hash buckets and tuples acquired in the prior pass,
	 * and reinitialize the context for a new pass.
	 */
	MemoryContextReset(hashtable->batchCxt);
	oldcxt = MemoryContextSwitchTo(hashtable->batchCxt);

	/*
	 * We still use the same number of physical buckets as in the first
	 * pass. (It could be different; but we already decided how many
	 * buckets would be appropriate for the allowed memory, so stick with
	 * that number.) We MUST set totalbuckets to equal nbuckets, because
	 * from now on no tuples will go out to temp files; there are no more
	 * virtual buckets, only real buckets.	(This implies that tuples will
	 * go into different bucket numbers than they did on the first pass,
	 * but that's OK.)
	 */
	hashtable->totalbuckets = nbuckets;

	/* Reallocate and reinitialize the hash bucket headers. */
	hashtable->buckets = (HashJoinTuple *)
		palloc0(nbuckets * sizeof(HashJoinTuple));

	MemoryContextSwitchTo(oldcxt);
}

void
ExecReScanHash(HashState *node, ExprContext *exprCtxt)
{
	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((PlanState *) node)->lefttree->chgParam == NULL)
		ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
}
