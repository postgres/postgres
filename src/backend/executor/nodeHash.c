/*-------------------------------------------------------------------------
 *
 * nodeHash.c
 *	  Routines to hash relations for hashjoin
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 *  $Id: nodeHash.c,v 1.34 1999/05/09 00:53:20 tgl Exp $
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
#include <stdio.h>		
#include <math.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <unistd.h>

#include "postgres.h"
#include "miscadmin.h"
#include "executor/execdebug.h"
#include "executor/executor.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "storage/ipc.h"
#include "utils/hsearch.h"

extern int	NBuffers;

static int	hashFunc(Datum key, int len, bool byVal);
static RelativeAddr hashTableAlloc(int size, HashJoinTable hashtable);
static void * absHashTableAlloc(int size, HashJoinTable hashtable);
static void ExecHashOverflowInsert(HashJoinTable hashtable,
					   HashBucket bucket,
					   HeapTuple heapTuple);

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
	File	   *batches = NULL;
	RelativeAddr *batchPos;
	int		   *batchSizes;
	int			i;

	/* ----------------
	 *	get state info from node
	 * ----------------
	 */

	hashstate = node->hashstate;
	estate = node->plan.state;
	outerNode = outerPlan(node);

	hashtable = node->hashtable;
	if (hashtable == NULL)
		elog(ERROR, "ExecHash: hash table is NULL.");

	nbatch = hashtable->nbatch;

	if (nbatch > 0)
	{							/* if needs hash partition */
		/* --------------
		 *	allocate space for the file descriptors of batch files
		 *	then open the batch files in the current processes.
		 * --------------
		 */
		batches = (File *) palloc(nbatch * sizeof(File));
		for (i = 0; i < nbatch; i++)
		{
			batches[i] = OpenTemporaryFile();
		}
		hashstate->hashBatches = batches;
		batchPos = (RelativeAddr *) ABSADDR(hashtable->innerbatchPos);
		batchSizes = (int *) ABSADDR(hashtable->innerbatchSizes);
	}

	/* ----------------
	 *	set expression context
	 * ----------------
	 */
	hashkey = node->hashkey;
	econtext = hashstate->cstate.cs_ExprContext;

	/* ----------------
	 *	get tuple and insert into the hash table
	 * ----------------
	 */
	for (;;)
	{
		slot = ExecProcNode(outerNode, (Plan *) node);
		if (TupIsNull(slot))
			break;

		econtext->ecxt_innertuple = slot;
		ExecHashTableInsert(hashtable, econtext, hashkey,
							hashstate->hashBatches);

		ExecClearTuple(slot);
	}

	/*
	 * end of build phase, flush all the last pages of the batches.
	 */
	for (i = 0; i < nbatch; i++)
	{
		if (FileSeek(batches[i], 0L, SEEK_END) < 0)
			perror("FileSeek");
		if (FileWrite(batches[i], ABSADDR(hashtable->batch) + i * BLCKSZ, BLCKSZ) < 0)
			perror("FileWrite");
		NDirectFileWrite++;
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
	hashstate->hashBatches = NULL;

	/* ----------------
	 *	Miscellanious initialization
	 *
	 *		 +	assign node's base_id
	 *		 +	assign debugging hooks and
	 *		 +	create expression context for node
	 * ----------------
	 */
	ExecAssignNodeBaseInfo(estate, &hashstate->cstate, parent);
	ExecAssignExprContext(estate, &hashstate->cstate);

#define HASH_NSLOTS 1
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
	File	   *batches;

	/* ----------------
	 *	get info from the hash state
	 * ----------------
	 */
	hashstate = node->hashstate;
	batches = hashstate->hashBatches;
	if (batches != NULL)
		pfree(batches);

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

static RelativeAddr
hashTableAlloc(int size, HashJoinTable hashtable)
{
	RelativeAddr p = hashtable->top;
	hashtable->top += MAXALIGN(size);
	return p;
}

static void *
absHashTableAlloc(int size, HashJoinTable hashtable)
{
	RelativeAddr p = hashTableAlloc(size, hashtable);
	return ABSADDR(p);
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
	int			HashTBSize;
	int			nbatch;
	int			ntuples;
	int			tupsize;
	int			pages;
	int			sqrtpages;
	IpcMemoryId shmid;
	HashJoinTable hashtable;
	HashBucket	bucket;
	int			nbuckets;
	int			totalbuckets;
	int			bucketsize;
	int			i;
	RelativeAddr *outerbatchPos;
	RelativeAddr *innerbatchPos;
	int		   *innerbatchSizes;

	/* ----------------
	 *	Get information about the size of the relation to be hashed
	 *	(it's the "outer" subtree of this node, but the inner relation of
	 *	the hashjoin).
	 *  Caution: this is only the planner's estimates, and so
	 *  can't be trusted too far.  Apply a healthy fudge factor.
	 * ----------------
	 */
	outerNode = outerPlan(node);
	ntuples = outerNode->plan_size;
	if (ntuples <= 0)			/* force a plausible size if no info */
		ntuples = 1000;
	tupsize = outerNode->plan_width + sizeof(HeapTupleData);
	pages = (int) ceil((double) ntuples * tupsize * FUDGE_FAC / BLCKSZ);

	/*
	 * Max hashtable size is NBuffers pages, but not less than
	 * sqrt(estimated inner rel size), so as to avoid horrible performance.
	 * XXX since the hashtable is not allocated in shared mem anymore,
	 * it would probably be more appropriate to drive this from -S than -B.
	 */
	sqrtpages = (int) ceil(sqrt((double) pages));
	HashTBSize = NBuffers;
	if (sqrtpages > HashTBSize)
		HashTBSize = sqrtpages;

	/*
	 * Count the number of hash buckets we want for the whole relation,
	 * and the number we can actually fit in the allowed memory.
	 * NOTE: FUDGE_FAC here determines the fraction of the hashtable space
	 * saved for overflow records.  Need a better approach...
	 */
	totalbuckets = (int) ceil((double) ntuples / NTUP_PER_BUCKET);
	bucketsize = MAXALIGN(NTUP_PER_BUCKET * tupsize + sizeof(*bucket));
	nbuckets = (int) ((HashTBSize * BLCKSZ) / (bucketsize * FUDGE_FAC));

	if (totalbuckets <= nbuckets)
	{
		/* We have enough space, so no batching.  In theory we could
		 * even reduce HashTBSize, but as long as we don't have a way
		 * to deal with overflow-space overrun, best to leave the
		 * extra space available for overflow.
		 */
		nbuckets = totalbuckets;
		nbatch = 0;
	}
	else
	{
		/* Need to batch; compute how many batches we want to use.
		 * Note that nbatch doesn't have to have anything to do with
		 * the ratio totalbuckets/nbuckets; in fact, it is the number
		 * of groups we will use for the part of the data that doesn't
		 * fall into the first nbuckets hash buckets.
		 */
		nbatch = (int) ceil((double) (pages - HashTBSize) / HashTBSize);
		if (nbatch <= 0)
			nbatch = 1;
	}

	/* Now, totalbuckets is the number of (virtual) hashbuckets for the
	 * whole relation, and nbuckets is the number of physical hashbuckets
	 * we will use in the first pass.  Data falling into the first nbuckets
	 * virtual hashbuckets gets handled in the first pass; everything else
	 * gets divided into nbatch batches to be processed in additional
	 * passes.
	 */
#ifdef HJDEBUG
	printf("nbatch = %d, totalbuckets = %d, nbuckets = %d\n", 
			nbatch, totalbuckets, nbuckets);
#endif

	/* ----------------
	 *	in non-parallel machines, we don't need to put the hash table
	 *	in the shared memory.  We just palloc it.  The space needed
	 *  is the hash area itself plus nbatch+1 I/O buffer pages.
	 * ----------------
	 */
	hashtable = (HashJoinTable) palloc((HashTBSize + nbatch + 1) * BLCKSZ);
	shmid = 0;

	if (hashtable == NULL)
		elog(ERROR, "not enough memory for hashjoin.");
	/* ----------------
	 *	initialize the hash table header
	 * ----------------
	 */
	hashtable->nbuckets = nbuckets;
	hashtable->totalbuckets = totalbuckets;
	hashtable->bucketsize = bucketsize;
	hashtable->shmid = shmid;
	hashtable->top = MAXALIGN(sizeof(HashTableData));
	hashtable->bottom = HashTBSize * BLCKSZ;
	/*
	 * hashtable->readbuf has to be maxaligned!!!
	 * Note there are nbatch additional pages available after readbuf;
	 * these are used for buffering the outgoing batch data.
	 */
	hashtable->readbuf = hashtable->bottom;
	hashtable->batch = hashtable->bottom + BLCKSZ;
	hashtable->nbatch = nbatch;
	hashtable->curbatch = 0;
	hashtable->pcount = hashtable->nprocess = 0;
	if (nbatch > 0)
	{
		/* ---------------
		 *	allocate and initialize the outer batches
		 * ---------------
		 */
		outerbatchPos = (RelativeAddr *)
			absHashTableAlloc(nbatch * sizeof(RelativeAddr), hashtable);
		for (i = 0; i < nbatch; i++)
		{
			outerbatchPos[i] = -1;
		}
		hashtable->outerbatchPos = RELADDR(outerbatchPos);
		/* ---------------
		 *	allocate and initialize the inner batches
		 * ---------------
		 */
		innerbatchPos = (RelativeAddr *)
			absHashTableAlloc(nbatch * sizeof(RelativeAddr), hashtable);
		innerbatchSizes = (int *)
			absHashTableAlloc(nbatch * sizeof(int), hashtable);
		for (i = 0; i < nbatch; i++)
		{
			innerbatchPos[i] = -1;
			innerbatchSizes[i] = 0;
		}
		hashtable->innerbatchPos = RELADDR(innerbatchPos);
		hashtable->innerbatchSizes = RELADDR(innerbatchSizes);
	}
	else
	{
		hashtable->outerbatchPos = (RelativeAddr) NULL;
		hashtable->innerbatchPos = (RelativeAddr) NULL;
		hashtable->innerbatchSizes = (RelativeAddr) NULL;
	}

	hashtable->overflownext = hashtable->top + bucketsize * nbuckets;
	Assert(hashtable->overflownext < hashtable->bottom);
	/* ----------------
	 *	initialize each hash bucket
	 * ----------------
	 */
	bucket = (HashBucket) ABSADDR(hashtable->top);
	for (i = 0; i < nbuckets; i++)
	{
		bucket->top = RELADDR((char *) bucket + MAXALIGN(sizeof(*bucket)));
		bucket->bottom = bucket->top;
		bucket->firstotuple = bucket->lastotuple = -1;
		bucket = (HashBucket) ((char *) bucket + bucketsize);
	}
	return hashtable;
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
					Var *hashkey,
					File *batches)
{
	TupleTableSlot *slot;
	HeapTuple	heapTuple;
	HashBucket	bucket;
	int			bucketno;
	int			nbatch;
	int			batchno;
	char	   *buffer;
	RelativeAddr *batchPos;
	int		   *batchSizes;
	char	   *pos;

	nbatch = hashtable->nbatch;
	batchPos = (RelativeAddr *) ABSADDR(hashtable->innerbatchPos);
	batchSizes = (int *) ABSADDR(hashtable->innerbatchSizes);

	slot = econtext->ecxt_innertuple;
	heapTuple = slot->val;

#ifdef HJDEBUG
	printf("Inserting ");
#endif

	bucketno = ExecHashGetBucket(hashtable, econtext, hashkey);

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
		bucket = (HashBucket)
			(ABSADDR(hashtable->top) + bucketno * hashtable->bucketsize);
		if (((char *) MAXALIGN(ABSADDR(bucket->bottom)) - (char *) bucket)
				+ heapTuple->t_len + HEAPTUPLESIZE > hashtable->bucketsize)
			ExecHashOverflowInsert(hashtable, bucket, heapTuple);
		else
		{
			memmove((char *) MAXALIGN(ABSADDR(bucket->bottom)),
					heapTuple,
					HEAPTUPLESIZE);
			memmove((char *) MAXALIGN(ABSADDR(bucket->bottom)) + HEAPTUPLESIZE,
					heapTuple->t_data,
					heapTuple->t_len);
			bucket->bottom = ((RelativeAddr) MAXALIGN(bucket->bottom) + 
					heapTuple->t_len + HEAPTUPLESIZE);
		}
	}
	else
	{
		/* -----------------
		 * put the tuple into a tmp file for other batches
		 * -----------------
		 */
		batchno = (nbatch * (bucketno - hashtable->nbuckets)) /
			(hashtable->totalbuckets - hashtable->nbuckets);
		buffer = ABSADDR(hashtable->batch) + batchno * BLCKSZ;
		batchSizes[batchno]++;
		pos = (char *)
			ExecHashJoinSaveTuple(heapTuple,
								  buffer,
								  batches[batchno],
								  (char *) ABSADDR(batchPos[batchno]));
		batchPos[batchno] = RELADDR(pos);
	}
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
	pfree(hashtable);
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
	 * ----------------
	 * ...It's quick hack - use ExecEvalExpr instead of ExecEvalVar:
	 * hashkey may be T_ArrayRef, not just T_Var.		- vadim 04/22/97
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
 *		ExecHashOverflowInsert
 *
 *		insert into the overflow area of a hash bucket
 * ----------------------------------------------------------------
 */
static void
ExecHashOverflowInsert(HashJoinTable hashtable,
					   HashBucket bucket,
					   HeapTuple heapTuple)
{
	OverflowTuple otuple;
	RelativeAddr newend;
	OverflowTuple firstotuple;
	OverflowTuple lastotuple;

	firstotuple = (OverflowTuple) ABSADDR(bucket->firstotuple);
	lastotuple = (OverflowTuple) ABSADDR(bucket->lastotuple);
	/* ----------------
	 *	see if we run out of overflow space
	 * ----------------
	 */
	newend = (RelativeAddr) MAXALIGN(hashtable->overflownext + sizeof(*otuple)
									  + heapTuple->t_len + HEAPTUPLESIZE);
	if (newend > hashtable->bottom)
		elog(ERROR, 
			 "hash table out of memory. Use -B parameter to increase buffers.");

	/* ----------------
	 *	establish the overflow chain
	 * ----------------
	 */
	otuple = (OverflowTuple) ABSADDR(hashtable->overflownext);
	hashtable->overflownext = newend;
	if (firstotuple == NULL)
		bucket->firstotuple = bucket->lastotuple = RELADDR(otuple);
	else
	{
		lastotuple->next = RELADDR(otuple);
		bucket->lastotuple = RELADDR(otuple);
	}

	/* ----------------
	 *	copy the tuple into the overflow area
	 * ----------------
	 */
	otuple->next = -1;
	otuple->tuple = RELADDR(MAXALIGN(((char *) otuple + sizeof(*otuple))));
	memmove(ABSADDR(otuple->tuple),
			heapTuple,
			HEAPTUPLESIZE);
	memmove(ABSADDR(otuple->tuple) + HEAPTUPLESIZE,
			heapTuple->t_data,
			heapTuple->t_len);
}

/* ----------------------------------------------------------------
 *		ExecScanHashBucket
 *
 *		scan a hash bucket of matches
 * ----------------------------------------------------------------
 */
HeapTuple
ExecScanHashBucket(HashJoinState *hjstate,
				   HashBucket bucket,
				   HeapTuple curtuple,
				   List *hjclauses,
				   ExprContext *econtext)
{
	HeapTuple	heapTuple;
	bool		qualResult;
	OverflowTuple otuple = NULL;
	OverflowTuple curotuple;
	TupleTableSlot *inntuple;
	OverflowTuple firstotuple;
	OverflowTuple lastotuple;
	HashJoinTable hashtable;

	hashtable = hjstate->hj_HashTable;
	firstotuple = (OverflowTuple) ABSADDR(bucket->firstotuple);
	lastotuple = (OverflowTuple) ABSADDR(bucket->lastotuple);

	/* ----------------
	 *	search the hash bucket
	 * ----------------
	 */
	if (curtuple == NULL || curtuple < (HeapTuple) ABSADDR(bucket->bottom))
	{
		if (curtuple == NULL)
			heapTuple = (HeapTuple)
				MAXALIGN(ABSADDR(bucket->top));
		else
			heapTuple = (HeapTuple)
				MAXALIGN(((char *) curtuple + curtuple->t_len + HEAPTUPLESIZE));

		while (heapTuple < (HeapTuple) ABSADDR(bucket->bottom))
		{

			heapTuple->t_data = (HeapTupleHeader) 
								((char *) heapTuple + HEAPTUPLESIZE);

			inntuple = ExecStoreTuple(heapTuple,		/* tuple to store */
									  hjstate->hj_HashTupleSlot,		/* slot */
									  InvalidBuffer,	/* tuple has no buffer */
									  false);	/* do not pfree this tuple */

			econtext->ecxt_innertuple = inntuple;
			qualResult = ExecQual((List *) hjclauses, econtext);

			if (qualResult)
				return heapTuple;

			heapTuple = (HeapTuple)
				MAXALIGN(((char *) heapTuple + heapTuple->t_len + HEAPTUPLESIZE));
		}

		if (firstotuple == NULL)
			return NULL;
		otuple = firstotuple;
	}

	/* ----------------
	 *	search the overflow area of the hash bucket
	 * ----------------
	 */
	if (otuple == NULL)
	{
		curotuple = hjstate->hj_CurOTuple;
		otuple = (OverflowTuple) ABSADDR(curotuple->next);
	}

	while (otuple != NULL)
	{
		heapTuple = (HeapTuple) ABSADDR(otuple->tuple);
		heapTuple->t_data = (HeapTupleHeader) 
							((char *) heapTuple + HEAPTUPLESIZE);

		inntuple = ExecStoreTuple(heapTuple,	/* tuple to store */
								  hjstate->hj_HashTupleSlot,	/* slot */
								  InvalidBuffer,		/* SP?? this tuple has
														 * no buffer */
								  false);		/* do not pfree this tuple */

		econtext->ecxt_innertuple = inntuple;
		qualResult = ExecQual((List *) hjclauses, econtext);

		if (qualResult)
		{
			hjstate->hj_CurOTuple = otuple;
			return heapTuple;
		}

		otuple = (OverflowTuple) ABSADDR(otuple->next);
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
	unsigned int	h = 0;
	unsigned char  *k;

	if (byVal) {
		/*
		 * If it's a by-value data type, use the 'len' least significant bytes
		 * of the Datum value.  This should do the right thing on either
		 * bigendian or littleendian hardware --- see the Datum access
		 * macros in c.h.
		 */
		while (len-- > 0) {
			h = (h * PRIME1) ^ (key & 0xFF);
			key >>= 8;
		}
	} else {
		/*
		 * If this is a variable length type, then 'k' points to a "struct
		 * varlena" and len == -1. NOTE: VARSIZE returns the "real" data
		 * length plus the sizeof the "vl_len" attribute of varlena (the
		 * length information). 'k' points to the beginning of the varlena
		 * struct, so we have to use "VARDATA" to find the beginning of the
		 * "real" data.
		 */
		if (len == -1)
		{
			len = VARSIZE(key) - VARHDRSZ;
			k = (unsigned char *) VARDATA(key);
		}
		else
		{
			k = (unsigned char *) key;
		}
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
 * ----------------------------------------------------------------
 */
void
ExecHashTableReset(HashJoinTable hashtable, int ntuples)
{
	int			i;
	HashBucket	bucket;

	/*
	 * We can reset the number of hashbuckets since we are going to
	 * recalculate the hash values of all the tuples in the new batch
	 * anyway.  We might as well spread out the hash values as much as
	 * we can within the available space.  Note we must set nbuckets
	 * equal to totalbuckets since we will NOT generate any new output
	 * batches after this point.
	 */
	hashtable->nbuckets = hashtable->totalbuckets =
		(int) (hashtable->bottom / (hashtable->bucketsize * FUDGE_FAC));

	/*
	 * reinitialize the overflow area to empty, and reinit each hash bucket.
	 */
	hashtable->overflownext = hashtable->top + hashtable->bucketsize *
		hashtable->nbuckets;
	Assert(hashtable->overflownext < hashtable->bottom);

	bucket = (HashBucket) ABSADDR(hashtable->top);
	for (i = 0; i < hashtable->nbuckets; i++)
	{
		bucket->top = RELADDR((char *) bucket + MAXALIGN(sizeof(*bucket)));
		bucket->bottom = bucket->top;
		bucket->firstotuple = bucket->lastotuple = -1;
		bucket = (HashBucket) ((char *) bucket + hashtable->bucketsize);
	}

	hashtable->pcount = hashtable->nprocess;
}

void
ExecReScanHash(Hash *node, ExprContext *exprCtxt, Plan *parent)
{
	HashState  *hashstate = node->hashstate;

	if (hashstate->hashBatches != NULL)
	{
		pfree(hashstate->hashBatches);
		hashstate->hashBatches = NULL;
	}

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((Plan *) node)->lefttree->chgParam == NULL)
		ExecReScan(((Plan *) node)->lefttree, exprCtxt, (Plan *) node);

}
