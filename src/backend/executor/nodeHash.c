/*-------------------------------------------------------------------------
 *
 * nodeHash.c--
 *    Routines to hash relations for hashjoin
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/executor/nodeHash.c,v 1.9 1997/07/28 00:53:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *     	ExecHash	- generate an in-memory hash table of the relation 
 *     	ExecInitHash	- initialize node and subnodes..
 *     	ExecEndHash	- shutdown node and subnodes
 *
 */

#include <sys/types.h>
#include <stdio.h>	/* for sprintf() */
#include <math.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <unistd.h>

#include "postgres.h"


#include "storage/fd.h"		/* for SEEK_ */
#include "storage/ipc.h"
#include "storage/bufmgr.h"	/* for BLCKSZ */
#include "executor/executor.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "executor/execdebug.h"
#include "utils/palloc.h"
#include "utils/hsearch.h"

extern int NBuffers;
static int HashTBSize;

static void mk_hj_temp(char *tempname);
static int hashFunc(char *key, int len);

/* ----------------------------------------------------------------
 *   	ExecHash
 *
 *	build hash table for hashjoin, all do partitioning if more
 *	than one batches are required.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecHash(Hash *node)
{
    EState	  *estate;
    HashState	  *hashstate;
    Plan 	  *outerNode;
    Var	  	  *hashkey;
    HashJoinTable hashtable;
    TupleTableSlot *slot;
    ExprContext	  *econtext;
    
    int		  nbatch;
    File	  *batches = NULL;
    RelativeAddr  *batchPos;
    int		  *batchSizes;
    int		  i;
    RelativeAddr  *innerbatchNames;
    
    /* ----------------
     *	get state info from node
     * ----------------
     */
    
    hashstate =   node->hashstate;
    estate =      node->plan.state;
    outerNode =   outerPlan(node);

    hashtable =	node->hashtable;
    if (hashtable == NULL)
	elog(WARN, "ExecHash: hash table is NULL.");
    
    nbatch = hashtable->nbatch;
    
    if (nbatch > 0) {  /* if needs hash partition */
	innerbatchNames = (RelativeAddr *) ABSADDR(hashtable->innerbatchNames);
	
	/* --------------
	 *  allocate space for the file descriptors of batch files
	 *  then open the batch files in the current processes.
	 * --------------
	 */
	batches = (File*)palloc(nbatch * sizeof(File));
	for (i=0; i<nbatch; i++) {
	    batches[i] = FileNameOpenFile(ABSADDR(innerbatchNames[i]),
					  O_CREAT | O_RDWR, 0600);
	}
	hashstate->hashBatches = batches;
        batchPos = (RelativeAddr*) ABSADDR(hashtable->innerbatchPos);
        batchSizes = (int*) ABSADDR(hashtable->innerbatchSizes);
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
    for (;;) {
	slot = ExecProcNode(outerNode, (Plan*)node);
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
    for (i=0; i<nbatch; i++) {
	if (FileSeek(batches[i], 0L, SEEK_END) < 0)
	    perror("FileSeek");
	if (FileWrite(batches[i],ABSADDR(hashtable->batch)+i*BLCKSZ,BLCKSZ) < 0)
	    perror("FileWrite");
	NDirectFileWrite++;
    }
    
    /* ---------------------
     *  Return the slot so that we have the tuple descriptor 
     *  when we need to save/restore them.  -Jeff 11 July 1991
     * ---------------------
     */
    return slot;
}

/* ----------------------------------------------------------------
 *   	ExecInitHash
 *
 *   	Init routine for Hash node
 * ----------------------------------------------------------------
 */
bool
ExecInitHash(Hash *node, EState *estate, Plan *parent)
{
    HashState		*hashstate;
    Plan		*outerPlan;
    
    SO1_printf("ExecInitHash: %s\n",
	       "initializing hash node");
    
    /* ----------------
     *  assign the node's execution state
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
     *  Miscellanious initialization
     *
     *	     +	assign node's base_id
     *       +	assign debugging hooks and
     *       +	create expression context for node
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
    ExecInitNode(outerPlan, estate, (Plan *)node);
    
    /* ----------------
     * 	initialize tuple type. no need to initialize projection
     *  info because this node doesn't do projections
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
 *   	ExecEndHash
 *
 *	clean up routine for Hash node
 * ----------------------------------------------------------------
 */
void
ExecEndHash(Hash *node)
{
    HashState		*hashstate;
    Plan		*outerPlan;
    File		*batches;
    
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
     *  because that came from the outer plan...
     * ----------------
     */
    ExecFreeProjectionInfo(&hashstate->cstate);
    
    /* ----------------
     *	shut down the subplan
     * ----------------
     */
    outerPlan = outerPlan(node);
    ExecEndNode(outerPlan, (Plan*)node);
} 

RelativeAddr
hashTableAlloc(int size, HashJoinTable hashtable)
{
    RelativeAddr p;
    p = hashtable->top;
    hashtable->top += size;
    return p;
}

/* ----------------------------------------------------------------
 *	ExecHashTableCreate
 *
 *	create a hashtable in shared memory for hashjoin.
 * ----------------------------------------------------------------
 */
#define NTUP_PER_BUCKET		10
#define FUDGE_FAC		1.5

HashJoinTable
ExecHashTableCreate(Hash *node)
{
    Plan	  *outerNode;
    int		  nbatch;
    int 	  ntuples;
    int 	  tupsize;
    IpcMemoryId   shmid;
    HashJoinTable hashtable;
    HashBucket 	  bucket;
    int 	  nbuckets;
    int		  totalbuckets;
    int 	  bucketsize;
    int 	  i;
    RelativeAddr  *outerbatchNames;
    RelativeAddr  *outerbatchPos;
    RelativeAddr  *innerbatchNames;
    RelativeAddr  *innerbatchPos;
    int		  *innerbatchSizes;
    RelativeAddr  tempname;
    
    nbatch = -1;
    HashTBSize = NBuffers/2;
    while (nbatch < 0) {
	/*
	 * determine number of batches for the hashjoin
	 */
	HashTBSize *= 2;
	nbatch = ExecHashPartition(node);
    }
    /* ----------------
     *	get information about the size of the relation
     * ----------------
     */
    outerNode = outerPlan(node);
    ntuples = outerNode->plan_size;
    if (ntuples <= 0)
	ntuples = 1000;  /* XXX just a hack */
    tupsize = outerNode->plan_width + sizeof(HeapTupleData);
    
    /*
     * totalbuckets is the total number of hash buckets needed for
     * the entire relation
     */
    totalbuckets = ceil((double)ntuples/NTUP_PER_BUCKET);
    bucketsize = LONGALIGN (NTUP_PER_BUCKET * tupsize + sizeof(*bucket));
    
    /*
     * nbuckets is the number of hash buckets for the first pass
     * of hybrid hashjoin
     */
    nbuckets = (HashTBSize - nbatch) * BLCKSZ / (bucketsize * FUDGE_FAC);
    if (totalbuckets < nbuckets)
	totalbuckets = nbuckets;
    if (nbatch == 0)
	nbuckets = totalbuckets;
#ifdef HJDEBUG
    printf("nbatch = %d, totalbuckets = %d, nbuckets = %d\n", nbatch, totalbuckets, nbuckets);
#endif
    
    /* ----------------
     *  in non-parallel machines, we don't need to put the hash table
     *  in the shared memory.  We just palloc it.
     * ----------------
     */
    hashtable = (HashJoinTable)palloc((HashTBSize+1)*BLCKSZ);
    shmid = 0;
    
    if (hashtable == NULL) {
	elog(WARN, "not enough memory for hashjoin.");
    }
    /* ----------------
     *	initialize the hash table header
     * ----------------
     */
    hashtable->nbuckets = nbuckets;
    hashtable->totalbuckets = totalbuckets;
    hashtable->bucketsize = bucketsize;
    hashtable->shmid = shmid;
    hashtable->top = sizeof(HashTableData);
    hashtable->bottom = HashTBSize * BLCKSZ;
    /*
     *  hashtable->readbuf has to be long aligned!!!
     */
    hashtable->readbuf = hashtable->bottom;
    hashtable->nbatch = nbatch;
    hashtable->curbatch = 0;
    hashtable->pcount = hashtable->nprocess = 0;
    if (nbatch > 0) {
	/* ---------------
	 *  allocate and initialize the outer batches
	 * ---------------
	 */
	outerbatchNames = (RelativeAddr*)ABSADDR(
						 hashTableAlloc(nbatch * sizeof(RelativeAddr), hashtable));
	outerbatchPos = (RelativeAddr*)ABSADDR(
					       hashTableAlloc(nbatch * sizeof(RelativeAddr), hashtable));
	for (i=0; i<nbatch; i++) {
	    tempname = hashTableAlloc(12, hashtable);
	    mk_hj_temp(ABSADDR(tempname));
	    outerbatchNames[i] = tempname;
	    outerbatchPos[i] = -1;
	}
	hashtable->outerbatchNames = RELADDR(outerbatchNames);
	hashtable->outerbatchPos = RELADDR(outerbatchPos);
	/* ---------------
	 *  allocate and initialize the inner batches
	 * ---------------
	 */
	innerbatchNames = (RelativeAddr*)ABSADDR(
						 hashTableAlloc(nbatch * sizeof(RelativeAddr), hashtable));
	innerbatchPos = (RelativeAddr*)ABSADDR(
					       hashTableAlloc(nbatch * sizeof(RelativeAddr), hashtable));
	innerbatchSizes = (int*)ABSADDR(
					hashTableAlloc(nbatch * sizeof(int), hashtable));
	for (i=0; i<nbatch; i++) {
	    tempname = hashTableAlloc(12, hashtable);
	    mk_hj_temp(ABSADDR(tempname));
	    innerbatchNames[i] = tempname;
	    innerbatchPos[i] = -1;
	    innerbatchSizes[i] = 0;
	}
	hashtable->innerbatchNames = RELADDR(innerbatchNames);
	hashtable->innerbatchPos = RELADDR(innerbatchPos);
	hashtable->innerbatchSizes = RELADDR(innerbatchSizes);
    }
    else {
	hashtable->outerbatchNames = (RelativeAddr)NULL;
	hashtable->outerbatchPos = (RelativeAddr)NULL;
	hashtable->innerbatchNames = (RelativeAddr)NULL;
	hashtable->innerbatchPos = (RelativeAddr)NULL;
	hashtable->innerbatchSizes = (RelativeAddr)NULL;
    }
    
    hashtable->batch = (RelativeAddr)LONGALIGN(hashtable->top + 
					       bucketsize * nbuckets);
    hashtable->overflownext=hashtable->batch + nbatch * BLCKSZ;
    /* ----------------
     *	initialize each hash bucket
     * ----------------
     */
    bucket = (HashBucket)ABSADDR(hashtable->top);
    for (i=0; i<nbuckets; i++) {
	bucket->top = RELADDR((char*)bucket + sizeof(*bucket));
	bucket->bottom = bucket->top;
	bucket->firstotuple = bucket->lastotuple = -1;
	bucket = (HashBucket)LONGALIGN(((char*)bucket + bucketsize));
    }
    return(hashtable);
}

/* ----------------------------------------------------------------
 *	ExecHashTableInsert
 *
 *	insert a tuple into the hash table depending on the hash value
 *	it may just go to a tmp file for other batches
 * ----------------------------------------------------------------
 */
void
ExecHashTableInsert(HashJoinTable hashtable,
		    ExprContext *econtext,
		    Var *hashkey,
		    File *batches)
{
    TupleTableSlot	*slot;
    HeapTuple 		heapTuple;
    HashBucket 		bucket;
    int			bucketno;
    int			nbatch;
    int			batchno;
    char		*buffer;
    RelativeAddr	*batchPos;
    int			*batchSizes;
    char		*pos;
    
    nbatch = 		hashtable->nbatch;
    batchPos = 		(RelativeAddr*)ABSADDR(hashtable->innerbatchPos);
    batchSizes = 	(int*)ABSADDR(hashtable->innerbatchSizes);
    
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
    if (bucketno < hashtable->nbuckets) {
	/* ---------------
	 *  put the tuple in hash table
	 * ---------------
	 */
	bucket = (HashBucket)
	    (ABSADDR(hashtable->top) + bucketno * hashtable->bucketsize);
	if ((char*)LONGALIGN(ABSADDR(bucket->bottom))
	    -(char*)bucket+heapTuple->t_len > hashtable->bucketsize)
	    ExecHashOverflowInsert(hashtable, bucket, heapTuple);
	else {
	    memmove((char*)LONGALIGN(ABSADDR(bucket->bottom)),
		    heapTuple,
		    heapTuple->t_len); 
	    bucket->bottom = 
		((RelativeAddr)LONGALIGN(bucket->bottom) + heapTuple->t_len);
	}
    }
    else {
	/* -----------------
	 * put the tuple into a tmp file for other batches
	 * -----------------
	 */
	batchno = (float)(bucketno - hashtable->nbuckets)/
	    (float)(hashtable->totalbuckets - hashtable->nbuckets)
		* nbatch;
	buffer = ABSADDR(hashtable->batch) + batchno * BLCKSZ;
	batchSizes[batchno]++;
	pos= (char *)
	    ExecHashJoinSaveTuple(heapTuple,
				  buffer,
				  batches[batchno],
				  (char*)ABSADDR(batchPos[batchno]));
	batchPos[batchno] = RELADDR(pos);
    }
}

/* ----------------------------------------------------------------
 *	ExecHashTableDestroy
 *
 *	destroy a hash table
 * ----------------------------------------------------------------
 */
void
ExecHashTableDestroy(HashJoinTable hashtable)
{
    pfree(hashtable);
}

/* ----------------------------------------------------------------
 *	ExecHashGetBucket
 *
 *	Get the hash value for a tuple
 * ----------------------------------------------------------------
 */
int
ExecHashGetBucket(HashJoinTable hashtable,
		  ExprContext *econtext,
		  Var *hashkey)
{
    int 	bucketno;
    Datum 	keyval;
    bool isNull;
    
    
    /* ----------------
     *	Get the join attribute value of the tuple
     * ----------------
     * ...It's quick hack - use ExecEvalExpr instead of ExecEvalVar:
     * hashkey may be T_ArrayRef, not just T_Var.	- vadim 04/22/97
     */
    keyval = ExecEvalExpr((Node*)hashkey, econtext, &isNull, NULL);
    
    /*
     * keyval could be null, so we better point it to something
     * valid before trying to run hashFunc on it. --djm 8/17/96
     */
    if(isNull) {
	execConstByVal = 0;
	execConstLen = 0;
	keyval = (Datum)"";
    }

    /* ------------------
     *  compute the hash function
     * ------------------
     */
    if (execConstByVal)
        bucketno =
	    hashFunc((char *) &keyval, execConstLen) % hashtable->totalbuckets;
    else
        bucketno =
	    hashFunc((char *) keyval, execConstLen) % hashtable->totalbuckets;
#ifdef HJDEBUG
    if (bucketno >= hashtable->nbuckets)
	printf("hash(%d) = %d SAVED\n", keyval, bucketno);
    else
	printf("hash(%d) = %d\n", keyval, bucketno);
#endif
    
    return(bucketno);
}

/* ----------------------------------------------------------------
 *	ExecHashOverflowInsert
 *
 *	insert into the overflow area of a hash bucket
 * ----------------------------------------------------------------
 */
void
ExecHashOverflowInsert(HashJoinTable hashtable,
		       HashBucket bucket,
		       HeapTuple heapTuple)
{
    OverflowTuple otuple;
    RelativeAddr  newend;
    OverflowTuple firstotuple;
    OverflowTuple lastotuple;
    
    firstotuple = (OverflowTuple)ABSADDR(bucket->firstotuple);
    lastotuple = (OverflowTuple)ABSADDR(bucket->lastotuple);
    /* ----------------
     *	see if we run out of overflow space
     * ----------------
     */
    newend = (RelativeAddr)LONGALIGN(hashtable->overflownext + sizeof(*otuple)
				     + heapTuple->t_len);
    if (newend > hashtable->bottom) {
#if 0
	elog(DEBUG, "hash table out of memory. expanding.");
	/* ------------------
	 * XXX this is a temporary hack
	 * eventually, recursive hash partitioning will be 
	 * implemented
	 * ------------------
	 */
	hashtable->readbuf = hashtable->bottom = 2 * hashtable->bottom;
	hashtable =
	    (HashJoinTable)repalloc(hashtable, hashtable->bottom+BLCKSZ);
	if (hashtable == NULL) {
	    perror("repalloc");
	    elog(WARN, "can't expand hashtable.");
	}
#else
      /* ------------------
       * XXX the temporary hack above doesn't work because things
       * above us don't know that we've moved the hash table!
       *  - Chris Dunlop, <chris@onthe.net.au>
       * ------------------
       */
      elog(WARN, "hash table out of memory. Use -B parameter to increase buffers.");
#endif

    }
    
    /* ----------------
     *	establish the overflow chain
     * ----------------
     */
    otuple = (OverflowTuple)ABSADDR(hashtable->overflownext);
    hashtable->overflownext = newend;
    if (firstotuple == NULL)
	bucket->firstotuple = bucket->lastotuple = RELADDR(otuple);
    else {
	lastotuple->next = RELADDR(otuple);
	bucket->lastotuple = RELADDR(otuple);
    }
    
    /* ----------------
     *	copy the tuple into the overflow area
     * ----------------
     */
    otuple->next = -1;
    otuple->tuple = RELADDR(LONGALIGN(((char*)otuple + sizeof(*otuple))));
    memmove(ABSADDR(otuple->tuple),
	    heapTuple,
	    heapTuple->t_len);
}

/* ----------------------------------------------------------------
 *	ExecScanHashBucket
 *
 *	scan a hash bucket of matches
 * ----------------------------------------------------------------
 */
HeapTuple
ExecScanHashBucket(HashJoinState *hjstate,
		   HashBucket bucket,
		   HeapTuple curtuple,
		   List *hjclauses,
		   ExprContext *econtext)
{
    HeapTuple 		heapTuple;
    bool 		qualResult;
    OverflowTuple 	otuple = NULL;
    OverflowTuple 	curotuple;
    TupleTableSlot	*inntuple;
    OverflowTuple	firstotuple;
    OverflowTuple	lastotuple;
    HashJoinTable	hashtable;
    
    hashtable = hjstate->hj_HashTable;
    firstotuple = (OverflowTuple)ABSADDR(bucket->firstotuple);
    lastotuple = (OverflowTuple)ABSADDR(bucket->lastotuple);
    
    /* ----------------
     *	search the hash bucket
     * ----------------
     */
    if (curtuple == NULL || curtuple < (HeapTuple)ABSADDR(bucket->bottom)) {
        if (curtuple == NULL)
	    heapTuple = (HeapTuple)
		LONGALIGN(ABSADDR(bucket->top));
	else 
	    heapTuple = (HeapTuple)
		LONGALIGN(((char*)curtuple+curtuple->t_len));
	
        while (heapTuple < (HeapTuple)ABSADDR(bucket->bottom)) {
	    
	    inntuple = 	ExecStoreTuple(heapTuple,	/* tuple to store */
				       hjstate->hj_HashTupleSlot,   /* slot */
				       InvalidBuffer,/* tuple has no buffer */
				       false);	/* do not pfree this tuple */
	    
	    econtext->ecxt_innertuple = inntuple;
	    qualResult = ExecQual((List*)hjclauses, econtext);
	    
	    if (qualResult)
		return heapTuple;
	    
	    heapTuple = (HeapTuple)
		LONGALIGN(((char*)heapTuple+heapTuple->t_len));
	}
	
	if (firstotuple == NULL)
	    return NULL;
	otuple = firstotuple;
    }
    
    /* ----------------
     *	search the overflow area of the hash bucket
     * ----------------
     */
    if (otuple == NULL) {
	curotuple = hjstate->hj_CurOTuple;
	otuple = (OverflowTuple)ABSADDR(curotuple->next);
    }
    
    while (otuple != NULL) {
	heapTuple = (HeapTuple)ABSADDR(otuple->tuple);
	
	inntuple =  ExecStoreTuple(heapTuple,	  /* tuple to store */
			   hjstate->hj_HashTupleSlot, /* slot */
			   InvalidBuffer, /* SP?? this tuple has no buffer */
			   false);	  /* do not pfree this tuple */
	
	econtext->ecxt_innertuple = inntuple;
	qualResult = ExecQual((List*)hjclauses, econtext);
	
	if (qualResult) {
	    hjstate->hj_CurOTuple = otuple;
	    return heapTuple;
	}
	
	otuple = (OverflowTuple)ABSADDR(otuple->next);
    }
    
    /* ----------------
     *	no match
     * ----------------
     */
    return NULL;
}

/* ----------------------------------------------------------------
 *	hashFunc
 *
 *	the hash function, copied from Margo
 * ----------------------------------------------------------------
 */
static int
hashFunc(char *key, int len)
{
    register unsigned int h;
    register int l;
    register unsigned char *k;
    
    /*
     * If this is a variable length type, then 'k' points
     * to a "struct varlena" and len == -1.
     * NOTE:
     * VARSIZE returns the "real" data length plus the sizeof the
     * "vl_len" attribute of varlena (the length information).
     * 'k' points to the beginning of the varlena struct, so
     * we have to use "VARDATA" to find the beginning of the "real"
     * data.
     */
    if (len == -1) {
	l = VARSIZE(key) - VARHDRSZ;
	k = (unsigned char*) VARDATA(key);
    } else {
	l = len;
	k = (unsigned char *) key;
    }
    
    h = 0;
    
    /*
     * Convert string to integer
     */
    while (l--) h = h * PRIME1 ^ (*k++);
    h %= PRIME2;
    
    return (h);
}

/* ----------------------------------------------------------------
 *	ExecHashPartition
 *
 *	determine the number of batches needed for a hashjoin
 * ----------------------------------------------------------------
 */
int
ExecHashPartition(Hash *node)
{
    Plan	*outerNode;
    int		b;
    int		pages;
    int		ntuples;
    int		tupsize;
    
    /*
     * get size information for plan node
     */
    outerNode = outerPlan(node);
    ntuples = outerNode->plan_size;
    if (ntuples == 0) ntuples = 1000;
    tupsize = outerNode->plan_width + sizeof(HeapTupleData);
    pages = ceil((double)ntuples * tupsize * FUDGE_FAC / BLCKSZ);
    
    /*
     * if amount of buffer space below hashjoin threshold,
     * return negative
     */
    if (ceil(sqrt((double)pages)) > HashTBSize)
	return -1;
    if (pages <= HashTBSize)
	b = 0;  /* fit in memory, no partitioning */
    else
	b = ceil((double)(pages - HashTBSize)/(double)(HashTBSize - 1));
    
    return b;
}

/* ----------------------------------------------------------------
 *	ExecHashTableReset
 *
 *	reset hash table header for new batch
 * ----------------------------------------------------------------
 */
void
ExecHashTableReset(HashJoinTable hashtable, int ntuples)
{
    int i;
    HashBucket bucket;
    
    hashtable->nbuckets = hashtable->totalbuckets
	= ceil((double)ntuples/NTUP_PER_BUCKET);
    
    hashtable->overflownext = hashtable->top + hashtable->bucketsize *
	hashtable->nbuckets;
    
    bucket = (HashBucket)ABSADDR(hashtable->top);
    for (i=0; i<hashtable->nbuckets; i++) {
	bucket->top = RELADDR((char*)bucket + sizeof(*bucket));
	bucket->bottom = bucket->top;
	bucket->firstotuple = bucket->lastotuple = -1;
	bucket = (HashBucket)((char*)bucket + hashtable->bucketsize);
    }
    hashtable->pcount = hashtable->nprocess;
}

static int hjtmpcnt = 0;

static void
mk_hj_temp(char *tempname)
{
    sprintf(tempname, "HJ%d.%d", (int)getpid(), hjtmpcnt);
    hjtmpcnt = (hjtmpcnt + 1) % 1000;
}



