/*-------------------------------------------------------------------------
 *
 * nodeHash.c
 *	  Routines to hash relations for hashjoin
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 *	$Id: nodeHash.c,v 1.40 1999/12/10 03:55:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecHash		- generate an in-memory hash table of the relation
 *		ExecInitHash	- initialize node and subnodes..
 *		ExecEndHash		- shutdown node and subnodes
 *
 */

#include <sys/types.h>
#include <math.h>

#include "postgres.h"
#include "executor/execdebug.h"
#include "executor/executor.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "utils/portal.h"

extern int	SortMem;

static int	hashFunc(Datum key, int len, bool byVal);

/* ----------------------------------------------------------------
 *		ExecHash
 *
 *		build hash table for hashjoin, all do partitioning if more
 *		than one batches are required.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecHash(Hash *node)
{
	EState	   *estate;
	HashState  *hashstate;
	Plan	   *outerNode;
	Var		   *hashkey;
	HashJoinTable hashtable;
	TupleTableSlot *slot;
	ExprContext *econtext;
	int			nbatch;
	int			i;

	/* ----------------
	 *	get state info from node
	 * ----------------
	 */

	hashstate = node->hashstate;
	estate = node->plan.state;
	outerNode = outerPlan(node);

	hashtable = hashstate->hashtable;
	if (hashtable == NULL)
		elog(ERROR, "ExecHash: hash table is NULL.");

	nbatch = hashtable->nbatch;

	if (nbatch > 0)
	{
		/* ----------------
		 * Open temp files for inner batches, if needed.
		 * Note that file buffers are palloc'd in regular executor context.
		 * ----------------
		 */
		for (i = 0; i < nbatch; i++)
			hashtable->innerBatchFile[i] = BufFileCreateTemp();
	}

	/* ----------------
	 *	set expression context
	 * ----------------
	 */
	hashkey = node->hashkey;
	econtext = hashstate->cstate.cs_ExprContext;

	/* ----------------
	 *	get all inner tuples and insert into the hash table (or temp files)
	 * ----------------
	 */
	for (;;)
	{
		slot = ExecProcNode(outerNode, (Plan *) node);
		if (TupIsNull(slot))
			break;
		econtext->ecxt_innertuple = slot;
		ExecHashTableInsert(hashtable, econtext, hashkey);
		ExecClearTuple(slot);
	}

	/* ---------------------
	 *	Return the slot so that we have the tuple descriptor
	 *	when we need to save/restore them.	-Jeff 11 July 1991
	 * ---------------------
	 */
	return slot;
}

/* ----------------------------------------------------------------
 *		ExecInitHash
 *
 *		Init routine for Hash node
 * ----------------------------------------------------------------
 */
bool
ExecInitHash(Hash *node, EState *estate, Plan *parent)
{
	HashState  *hashstate;
	Plan	   *outerPlan;

	SO1_printf("ExecInitHash: %s\n",
			   "initializing hash node");

	/* ----------------
	 *	assign the node's execution state
	 * ----------------
	 */
	node->plan.state = estate;

	/* ----------------
	 * create state structure
	 * ----------------
	 */
	hashstate = makeNode(HashState);
	node->hashstate = hashstate;
	hashstate->hashtable = NULL;

	/* ----------------
	 *	Miscellaneous initialization
	 *
	 *		 +	assign node's base_id
	 *		 +	assign debugging hooks and
	 *		 +	create expression context for node
	 * ----------------
	 */
	ExecAssignNodeBaseInfo(estate, &hashstate->cstate, parent);
	ExecAssignExprContext(estate, &hashstate->cstate);

	/* ----------------
	 * initialize our result slot
	 * ----------------
	 */
	ExecInitResultTupleSlot(estate, &hashstate->cstate);

	/* ----------------
	 * initializes child nodes
	 * ----------------
	 */
	outerPlan = outerPlan(node);
	ExecInitNode(outerPlan, estate, (Plan *) node);

	/* ----------------
	 *	initialize tuple type. no need to initialize projection
	 *	info because this node doesn't do projections
	 * ----------------
	 */
	ExecAssignResultTypeFromOuterPlan((Plan *) node, &hashstate->cstate);
	hashstate->cstate.cs_ProjInfo = NULL;

	return TRUE;
}

int
ExecCountSlotsHash(Hash *node)
{
#define HASH_NSLOTS 1
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
ExecEndHash(Hash *node)
{
	HashState  *hashstate;
	Plan	   *outerPlan;

	/* ----------------
	 *	get info from the hash state
	 * ----------------
	 */
	hashstate = node->hashstate;

	/* ----------------
	 *	free projection info.  no need to free result type info
	 *	because that came from the outer plan...
	 * ----------------
	 */
	ExecFreeProjectionInfo(&hashstate->cstate);

	/* ----------------
	 *	shut down the subplan
	 * ----------------
	 */
	outerPlan = outerPlan(node);
	ExecEndNode(outerPlan, (Plan *) node);
}


/* ----------------------------------------------------------------
 *		ExecHashTableCreate
 *
 *		create a hashtable in shared memory for hashjoin.
 * ----------------------------------------------------------------
 */
#define NTUP_PER_BUCKET			10
#define FUDGE_FAC				2.0

HashJoinTable
ExecHashTableCreate(Hash *node)
{
	Plan	   *outerNode;
	int			ntuples;
	int			tupsize;
	double		inner_rel_bytes;
	double		hash_table_bytes;
	int			nbatch;
	HashJoinTable hashtable;
	int			nbuckets;
	int			totalbuckets;
	int			bucketsize;
	int			i;
	Portal		myPortal;
	char		myPortalName[64];
	MemoryContext oldcxt;

	/* ----------------
	 *	Get information about the size of the relation to be hashed
	 *	(it's the "outer" subtree of this node, but the inner relation of
	 *	the hashjoin).
	 *	Caution: this is only the planner's estimates, and so
	 *	can't be trusted too far.  Apply a healthy fudge factor.
	 * ----------------
	 */
	outerNode = outerPlan(node);
	ntuples = outerNode->plan_size;
	if (ntuples <= 0)			/* force a plausible size if no info */
		ntuples = 1000;

	/*
	 * estimate tupsize based on footprint of tuple in hashtable... but
	 * what about palloc overhead?
	 */
	tupsize = MAXALIGN(outerNode->plan_width) +
		MAXALIGN(sizeof(HashJoinTupleData));
	inner_rel_bytes = (double) ntuples *tupsize * FUDGE_FAC;

	/*
	 * Target hashtable size is SortMem kilobytes, but not less than
	 * sqrt(estimated inner rel size), so as to avoid horrible
	 * performance.
	 */
	hash_table_bytes = sqrt(inner_rel_bytes);
	if (hash_table_bytes < (SortMem * 1024L))
		hash_table_bytes = SortMem * 1024L;

	/*
	 * Count the number of hash buckets we want for the whole relation,
	 * for an average bucket load of NTUP_PER_BUCKET (per virtual
	 * bucket!).
	 */
	totalbuckets = (int) ceil((double) ntuples * FUDGE_FAC / NTUP_PER_BUCKET);

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
		 * first nbuckets hash buckets.
		 */
		nbatch = (int) ceil((inner_rel_bytes - hash_table_bytes) /
							hash_table_bytes);
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
#ifdef HJDEBUG
	printf("nbatch = %d, totalbuckets = %d, nbuckets = %d\n",
		   nbatch, totalbuckets, nbuckets);
#endif

	/* ----------------
	 *	Initialize the hash table control block.
	 *	The hashtable control block is just palloc'd from executor memory.
	 * ----------------
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

	/* ----------------
	 *	Create a named portal in which to keep the hashtable working storage.
	 *	Each hashjoin must have its own portal, so be wary of name conflicts.
	 * ----------------
	 */
	i = 0;
	do
	{
		i++;
		sprintf(myPortalName, "<hashtable %d>", i);
		myPortal = GetPortalByName(myPortalName);
	} while (PortalIsValid(myPortal));
	myPortal = CreatePortal(myPortalName);
	Assert(PortalIsValid(myPortal));
	hashtable->myPortal = (void *) myPortal;	/* kluge for circular
												 * includes */
	hashtable->hashCxt = (MemoryContext) PortalGetVariableMemory(myPortal);
	hashtable->batchCxt = (MemoryContext) PortalGetHeapMemory(myPortal);

	/* Allocate data that will live for the life of the hashjoin */

	oldcxt = MemoryContextSwitchTo(hashtable->hashCxt);

	if (nbatch > 0)
	{
		/* ---------------
		 *	allocate and initialize the file arrays in hashCxt
		 * ---------------
		 */
		hashtable->innerBatchFile = (BufFile **)
			palloc(nbatch * sizeof(BufFile *));
		hashtable->outerBatchFile = (BufFile **)
			palloc(nbatch * sizeof(BufFile *));
		hashtable->innerBatchSize = (long *)
			palloc(nbatch * sizeof(long));
		hashtable->outerBatchSize = (long *)
			palloc(nbatch * sizeof(long));
		for (i = 0; i < nbatch; i++)
		{
			hashtable->innerBatchFile[i] = NULL;
			hashtable->outerBatchFile[i] = NULL;
			hashtable->innerBatchSize[i] = 0;
			hashtable->outerBatchSize[i] = 0;
		}
		/* The files will not be opened until later... */
	}

	/*
	 * Prepare portal for the first-scan space allocations; allocate the
	 * hashbucket array therein, and set each bucket "empty".
	 */
	MemoryContextSwitchTo(hashtable->batchCxt);
	StartPortalAllocMode(DefaultAllocMode, 0);

	hashtable->buckets = (HashJoinTuple *)
		palloc(nbuckets * sizeof(HashJoinTuple));

	if (hashtable->buckets == NULL)
		elog(ERROR, "Insufficient memory for hash table.");

	for (i = 0; i < nbuckets; i++)
		hashtable->buckets[i] = NULL;

	MemoryContextSwitchTo(oldcxt);

	return hashtable;
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

	/* Destroy the portal to release all working memory */
	/* cast here is a kluge for circular includes... */
	PortalDrop((Portal *) &hashtable->myPortal);

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
					Var *hashkey)
{
	int			bucketno = ExecHashGetBucket(hashtable, econtext, hashkey);
	TupleTableSlot *slot = econtext->ecxt_innertuple;
	HeapTuple	heapTuple = slot->val;

	/* ----------------
	 *	decide whether to put the tuple in the hash table or a tmp file
	 * ----------------
	 */
	if (bucketno < hashtable->nbuckets)
	{
		/* ---------------
		 *	put the tuple in hash table
		 * ---------------
		 */
		HashJoinTuple hashTuple;
		int			hashTupleSize;

		hashTupleSize = MAXALIGN(sizeof(*hashTuple)) + heapTuple->t_len;
		hashTuple = (HashJoinTuple) MemoryContextAlloc(hashtable->batchCxt,
													   hashTupleSize);
		if (hashTuple == NULL)
			elog(ERROR, "Insufficient memory for hash table.");
		memcpy((char *) &hashTuple->htup,
			   (char *) heapTuple,
			   sizeof(hashTuple->htup));
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
		/* -----------------
		 * put the tuple into a tmp file for other batches
		 * -----------------
		 */
		int			batchno = (hashtable->nbatch * (bucketno - hashtable->nbuckets)) /
		(hashtable->totalbuckets - hashtable->nbuckets);

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
				  Var *hashkey)
{
	int			bucketno;
	Datum		keyval;
	bool		isNull;

	/* ----------------
	 *	Get the join attribute value of the tuple
	 *
	 * ...It's quick hack - use ExecEvalExpr instead of ExecEvalVar:
	 * hashkey may be T_ArrayRef, not just T_Var.		- vadim 04/22/97
	 * ----------------
	 */
	keyval = ExecEvalExpr((Node *) hashkey, econtext, &isNull, NULL);

	/*
	 * keyval could be null, so we better point it to something valid
	 * before trying to run hashFunc on it. --djm 8/17/96
	 */
	if (isNull)
	{
		execConstByVal = 0;
		execConstLen = 0;
		keyval = (Datum) "";
	}

	/* ------------------
	 *	compute the hash function
	 * ------------------
	 */
	bucketno = hashFunc(keyval, execConstLen, execConstByVal) % hashtable->totalbuckets;

#ifdef HJDEBUG
	if (bucketno >= hashtable->nbuckets)
		printf("hash(%d) = %d SAVED\n", keyval, bucketno);
	else
		printf("hash(%d) = %d\n", keyval, bucketno);
#endif

	return bucketno;
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
		bool		qualResult;

		/* insert hashtable's tuple into exec slot so ExecQual sees it */
		inntuple = ExecStoreTuple(heapTuple,	/* tuple to store */
								  hjstate->hj_HashTupleSlot,	/* slot */
								  InvalidBuffer,
								  false);		/* do not pfree this tuple */
		econtext->ecxt_innertuple = inntuple;

		qualResult = ExecQual(hjclauses, econtext);

		if (qualResult)
		{
			hjstate->hj_CurTuple = hashTuple;
			return heapTuple;
		}

		hashTuple = hashTuple->next;
	}

	/* ----------------
	 *	no match
	 * ----------------
	 */
	return NULL;
}

/* ----------------------------------------------------------------
 *		hashFunc
 *
 *		the hash function, copied from Margo
 * ----------------------------------------------------------------
 */
static int
hashFunc(Datum key, int len, bool byVal)
{
	unsigned int h = 0;
	unsigned char *k;

	if (byVal)
	{

		/*
		 * If it's a by-value data type, use the 'len' least significant
		 * bytes of the Datum value.  This should do the right thing on
		 * either bigendian or littleendian hardware --- see the Datum
		 * access macros in c.h.
		 */
		while (len-- > 0)
		{
			h = (h * PRIME1) ^ (key & 0xFF);
			key >>= 8;
		}
	}
	else
	{

		/*
		 * If this is a variable length type, then 'k' points to a "struct
		 * varlena" and len == -1. NOTE: VARSIZE returns the "real" data
		 * length plus the sizeof the "vl_len" attribute of varlena (the
		 * length information). 'k' points to the beginning of the varlena
		 * struct, so we have to use "VARDATA" to find the beginning of
		 * the "real" data.
		 */
		if (len == -1)
		{
			len = VARSIZE(key) - VARHDRSZ;
			k = (unsigned char *) VARDATA(key);
		}
		else
			k = (unsigned char *) key;
		while (len-- > 0)
			h = (h * PRIME1) ^ (*k++);
	}

	return h % PRIME2;
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
	int			i;

	/*
	 * Release all the hash buckets and tuples acquired in the prior pass,
	 * and reinitialize the portal for a new pass.
	 */
	oldcxt = MemoryContextSwitchTo(hashtable->batchCxt);
	EndPortalAllocMode();
	StartPortalAllocMode(DefaultAllocMode, 0);

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
		palloc(nbuckets * sizeof(HashJoinTuple));

	if (hashtable->buckets == NULL)
		elog(ERROR, "Insufficient memory for hash table.");

	for (i = 0; i < nbuckets; i++)
		hashtable->buckets[i] = NULL;

	MemoryContextSwitchTo(oldcxt);
}

void
ExecReScanHash(Hash *node, ExprContext *exprCtxt, Plan *parent)
{

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((Plan *) node)->lefttree->chgParam == NULL)
		ExecReScan(((Plan *) node)->lefttree, exprCtxt, (Plan *) node);
}
