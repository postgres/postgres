/*-------------------------------------------------------------------------
 *
 * nodeHashjoin.c--
 *    Routines to handle hash join nodes
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/executor/nodeHashjoin.c,v 1.4 1997/07/28 00:54:06 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>



#include "postgres.h"
#include "storage/bufmgr.h"	/* for BLCKSZ */
#include "storage/fd.h"		/* for SEEK_ */
#include "executor/executor.h"
#include "executor/execdebug.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"

#include "optimizer/clauses.h"	/* for get_leftop */


#include "utils/palloc.h"

static TupleTableSlot *
ExecHashJoinOuterGetTuple(Plan *node, Plan* parent, HashJoinState *hjstate);

static TupleTableSlot *
ExecHashJoinGetSavedTuple(HashJoinState *hjstate, char *buffer,
	File file, TupleTableSlot *tupleSlot, int *block, char **position);

/* ----------------------------------------------------------------
 *   	ExecHashJoin
 *
 *	This function implements the Hybrid Hashjoin algorithm.
 *	recursive partitioning remains to be added.
 *	Note: the relation we build hash table on is the inner
 *	      the other one is outer.
 * ----------------------------------------------------------------
 */
TupleTableSlot *			/* return: a tuple or NULL */
ExecHashJoin(HashJoin *node)
{
    HashJoinState	*hjstate;
    EState		*estate;
    Plan 	  	*outerNode;
    Hash		*hashNode;
    List		*hjclauses;
    Expr		*clause;
    List		*qual;
    ScanDirection 	dir;
    TupleTableSlot	*inntuple;
    Var			*outerVar;
    ExprContext		*econtext;
    
    HashJoinTable	hashtable;
    int			bucketno;
    HashBucket		bucket;
    HeapTuple		curtuple;
    
    bool		qualResult;
    
    TupleTableSlot	*outerTupleSlot;
    TupleTableSlot	*innerTupleSlot;
    int			nbatch;
    int			curbatch;
    File		*outerbatches;
    RelativeAddr	*outerbatchNames;
    RelativeAddr	*outerbatchPos;
    Var			*innerhashkey;
    int			batch;
    int			batchno;
    char		*buffer;
    int			i;
    bool		hashPhaseDone;
    char		*pos;
    
    /* ----------------
     *	get information from HashJoin node
     * ----------------
     */
    hjstate =   	node->hashjoinstate;
    hjclauses = 	node->hashclauses;
    clause =		lfirst(hjclauses);
    estate = 		node->join.state;
    qual = 		node->join.qual;
    hashNode = 		(Hash *)innerPlan(node);
    outerNode = 	outerPlan(node);
    hashPhaseDone = 	node->hashdone;
    
    dir =   	  	estate->es_direction;
    
    /* -----------------
     * get information from HashJoin state
     * -----------------
     */
    hashtable = 	hjstate->hj_HashTable;
    bucket = 		hjstate->hj_CurBucket;
    curtuple =		hjstate->hj_CurTuple;
    
    /* --------------------
     * initialize expression context
     * --------------------
     */
    econtext = 	hjstate->jstate.cs_ExprContext;
    
    if (hjstate->jstate.cs_TupFromTlist) {
	TupleTableSlot  *result;
	bool		isDone;
	
	result = ExecProject(hjstate->jstate.cs_ProjInfo, &isDone);
	if (!isDone)
	    return result;
    }
    /* ----------------
     *	if this is the first call, build the hash table for inner relation
     * ----------------
     */
    if (!hashPhaseDone) {  /* if the hash phase not completed */
	hashtable = node->hashjointable;
        if (hashtable == NULL) { /* if the hash table has not been created */
	    /* ----------------
	     * create the hash table
	     * ----------------
	     */
	    hashtable = ExecHashTableCreate(hashNode);
	    hjstate->hj_HashTable = hashtable;
	    innerhashkey = hashNode->hashkey;
	    hjstate->hj_InnerHashKey = innerhashkey;
	    
	    /* ----------------
	     * execute the Hash node, to build the hash table 
	     * ----------------
	     */
	    hashNode->hashtable = hashtable;
	    innerTupleSlot = ExecProcNode((Plan *)hashNode, (Plan*) node);
	}
	bucket = NULL;
	curtuple = NULL;
	curbatch = 0;
	node->hashdone = true;
    }
    nbatch = hashtable->nbatch;
    outerbatches = hjstate->hj_OuterBatches;
    if (nbatch > 0 && outerbatches == NULL) {  /* if needs hash partition */
	/* -----------------
	 *  allocate space for file descriptors of outer batch files
	 *  then open the batch files in the current process
	 * -----------------
	 */
	innerhashkey = hashNode->hashkey;
	hjstate->hj_InnerHashKey = innerhashkey;
        outerbatchNames = (RelativeAddr*)
	    ABSADDR(hashtable->outerbatchNames);
	outerbatches = (File*)
	    palloc(nbatch * sizeof(File));
	for (i=0; i<nbatch; i++) {
	    outerbatches[i] = FileNameOpenFile(
					       ABSADDR(outerbatchNames[i]), 
					       O_CREAT | O_RDWR, 0600);
	}
	hjstate->hj_OuterBatches = outerbatches;

	/* ------------------
	 *  get the inner batch file descriptors from the
	 *  hash node
	 * ------------------
	 */
	hjstate->hj_InnerBatches =
	    hashNode->hashstate->hashBatches;
    }
    outerbatchPos = (RelativeAddr*)ABSADDR(hashtable->outerbatchPos);
    curbatch = hashtable->curbatch;
    outerbatchNames = (RelativeAddr*)ABSADDR(hashtable->outerbatchNames);
    
    /* ----------------
     *	Now get an outer tuple and probe into the hash table for matches
     * ----------------
     */
    outerTupleSlot = 	hjstate->jstate.cs_OuterTupleSlot;
    outerVar =   	get_leftop(clause);
    
    bucketno = -1;  /* if bucketno remains -1, means use old outer tuple */
    if (TupIsNull(outerTupleSlot)) {
	/*
	 * if the current outer tuple is nil, get a new one
	 */
	outerTupleSlot = (TupleTableSlot*)
	    ExecHashJoinOuterGetTuple(outerNode, (Plan*)node, hjstate);
	
	while (curbatch <= nbatch && TupIsNull(outerTupleSlot)) {
	    /*
	     * if the current batch runs out, switch to new batch
	     */
	    curbatch = ExecHashJoinNewBatch(hjstate);
	    if (curbatch > nbatch) {
		/*
		 * when the last batch runs out, clean up
		 */
		ExecHashTableDestroy(hashtable);
		hjstate->hj_HashTable = NULL;
		return NULL;
	    }
	    else
		outerTupleSlot = (TupleTableSlot*)
		    ExecHashJoinOuterGetTuple(outerNode, (Plan*)node, hjstate);
	}
	/*
	 * now we get an outer tuple, find the corresponding bucket for
	 * this tuple from the hash table
	 */
	econtext->ecxt_outertuple = outerTupleSlot;
	
#ifdef HJDEBUG
	printf("Probing ");
#endif
	bucketno = ExecHashGetBucket(hashtable, econtext, outerVar);
	bucket=(HashBucket)(ABSADDR(hashtable->top) 
			    + bucketno * hashtable->bucketsize);
    }
    
    for (;;) {
	/* ----------------
	 *	Now we've got an outer tuple and the corresponding hash bucket,
	 *  but this tuple may not belong to the current batch.
	 * ----------------
	 */
	if (curbatch == 0 && bucketno != -1)  /* if this is the first pass */
	    batch = ExecHashJoinGetBatch(bucketno, hashtable, nbatch);
	else
	    batch = 0;
	if (batch > 0) {
	    /*
	     * if the current outer tuple does not belong to
	     * the current batch, save to the tmp file for
	     * the corresponding batch.
	     */
	    buffer = ABSADDR(hashtable->batch) + (batch - 1) * BLCKSZ;
	    batchno = batch - 1;
	    pos  = ExecHashJoinSaveTuple(outerTupleSlot->val,
					 buffer,
					 outerbatches[batchno],
					 ABSADDR(outerbatchPos[batchno]));
	    
	    outerbatchPos[batchno] = RELADDR(pos);
	}
	else if (bucket != NULL) {
	    do {
		/*
		 * scan the hash bucket for matches
		 */
		curtuple = ExecScanHashBucket(hjstate,
					      bucket,
					      curtuple,
					      hjclauses,
					      econtext);
		
		if (curtuple != NULL) {
		    /*
		     * we've got a match, but still need to test qpqual
		     */
                    inntuple = ExecStoreTuple(curtuple, 
					      hjstate->hj_HashTupleSlot,
					      InvalidBuffer,
					      false); /* don't pfree this tuple */
		    
		    econtext->ecxt_innertuple = inntuple;
		    
		    /* ----------------
		     * test to see if we pass the qualification
		     * ----------------
		     */
		    qualResult = ExecQual((List*)qual, econtext);
		    
		    /* ----------------
		     * if we pass the qual, then save state for next call and
		     * have ExecProject form the projection, store it
		     * in the tuple table, and return the slot.
		     * ----------------
		     */
		    if (qualResult) {
			ProjectionInfo	*projInfo;
			TupleTableSlot  *result;
			bool            isDone;
			
			hjstate->hj_CurBucket = bucket;
			hjstate->hj_CurTuple = curtuple;
			hashtable->curbatch = curbatch;
			hjstate->jstate.cs_OuterTupleSlot = outerTupleSlot;
			
			projInfo = hjstate->jstate.cs_ProjInfo;
			result = ExecProject(projInfo, &isDone);
			hjstate->jstate.cs_TupFromTlist = !isDone;
			return result;
		    }
		}
	    }
	    while (curtuple != NULL);
	}
	
	/* ----------------
	 *   Now the current outer tuple has run out of matches,
	 *   so we free it and get a new outer tuple.
	 * ----------------
	 */
	outerTupleSlot = (TupleTableSlot*)
	    ExecHashJoinOuterGetTuple(outerNode, (Plan*) node, hjstate);
	
	while (curbatch <= nbatch && TupIsNull(outerTupleSlot)) {
	    /*
	     * if the current batch runs out, switch to new batch
	     */
	    curbatch = ExecHashJoinNewBatch(hjstate);
	    if (curbatch > nbatch) {
		/*
		 * when the last batch runs out, clean up
		 */
		ExecHashTableDestroy(hashtable);
		hjstate->hj_HashTable = NULL;
		return NULL;
	    }
	    else
		outerTupleSlot = (TupleTableSlot*)
		    ExecHashJoinOuterGetTuple(outerNode, (Plan*)node, hjstate);
	}
	
	/* ----------------
	 *   Now get the corresponding hash bucket for the new
	 *   outer tuple.
	 * ----------------
	 */
	econtext->ecxt_outertuple = outerTupleSlot;
#ifdef HJDEBUG
	printf("Probing ");
#endif
	bucketno = ExecHashGetBucket(hashtable, econtext, outerVar);
	bucket=(HashBucket)(ABSADDR(hashtable->top) 
			    + bucketno * hashtable->bucketsize);
	curtuple = NULL;
    }
}

/* ----------------------------------------------------------------
 *   	ExecInitHashJoin
 *
 *	Init routine for HashJoin node.
 * ----------------------------------------------------------------
 */
bool	/* return: initialization status */
ExecInitHashJoin(HashJoin *node, EState *estate, Plan *parent)
{
    HashJoinState	*hjstate;
    Plan 	  	*outerNode;
    Hash		*hashNode;
    
    /* ----------------
     *  assign the node's execution state
     * ----------------
     */
    node->join.state = estate;
    
    /* ----------------
     * create state structure
     * ----------------
     */
    hjstate = makeNode(HashJoinState);
    
    node->hashjoinstate = hjstate;
    
    /* ----------------
     *  Miscellanious initialization
     *
     *	     +	assign node's base_id
     *       +	assign debugging hooks and
     *       +	create expression context for node
     * ----------------
     */
    ExecAssignNodeBaseInfo(estate, &hjstate->jstate, parent);
    ExecAssignExprContext(estate, &hjstate->jstate);
    
#define HASHJOIN_NSLOTS 2
    /* ----------------
     *	tuple table initialization
     * ----------------
     */
    ExecInitResultTupleSlot(estate, &hjstate->jstate);
    ExecInitOuterTupleSlot(estate,  hjstate);    
    
    /* ----------------
     * initializes child nodes
     * ----------------
     */
    outerNode = outerPlan((Plan *)node);
    hashNode  = (Hash*)innerPlan((Plan *)node);
    
    ExecInitNode(outerNode, estate, (Plan *) node);
    ExecInitNode((Plan*)hashNode,  estate, (Plan *) node);
    
    /* ----------------
     *	now for some voodoo.  our temporary tuple slot
     *  is actually the result tuple slot of the Hash node
     *  (which is our inner plan).  we do this because Hash
     *  nodes don't return tuples via ExecProcNode() -- instead
     *  the hash join node uses ExecScanHashBucket() to get
     *  at the contents of the hash table.  -cim 6/9/91
     * ----------------
     */
    {
	HashState      *hashstate  = hashNode->hashstate;
	TupleTableSlot *slot 	  =
	    hashstate->cstate.cs_ResultTupleSlot;
	hjstate->hj_HashTupleSlot = slot;
    }
    hjstate->hj_OuterTupleSlot->ttc_tupleDescriptor = 
				ExecGetTupType(outerNode);
    
/*
    hjstate->hj_OuterTupleSlot->ttc_execTupDescriptor =
			      ExecGetExecTupDesc(outerNode);
*/
    
    /* ----------------
     * 	initialize tuple type and projection info
     * ----------------
     */
    ExecAssignResultTypeFromTL((Plan*) node, &hjstate->jstate);
    ExecAssignProjectionInfo((Plan*) node, &hjstate->jstate);
    
    /* ----------------
     *	XXX comment me
     * ----------------
     */
    
    node->hashdone = false;
    
    hjstate->hj_HashTable = (HashJoinTable)NULL;
    hjstate->hj_HashTableShmId = (IpcMemoryId)0;
    hjstate->hj_CurBucket = (HashBucket )NULL;
    hjstate->hj_CurTuple = (HeapTuple )NULL;
    hjstate->hj_CurOTuple = (OverflowTuple )NULL;
    hjstate->hj_InnerHashKey = (Var*)NULL;
    hjstate->hj_OuterBatches = (File*)NULL;
    hjstate->hj_InnerBatches = (File*)NULL;
    hjstate->hj_OuterReadPos = (char*)NULL;
    hjstate->hj_OuterReadBlk = (int)0;

    hjstate->jstate.cs_OuterTupleSlot = (TupleTableSlot*) NULL;
    hjstate->jstate.cs_TupFromTlist = (bool) false;
    
    return TRUE;
}

int
ExecCountSlotsHashJoin(HashJoin *node)
{
    return ExecCountSlotsNode(outerPlan(node)) +
	ExecCountSlotsNode(innerPlan(node)) +
	    HASHJOIN_NSLOTS;
}

/* ----------------------------------------------------------------
 *   	ExecEndHashJoin
 *
 *   	clean up routine for HashJoin node
 * ----------------------------------------------------------------
 */
void
ExecEndHashJoin(HashJoin *node)
{
    HashJoinState   *hjstate;
    
    /* ----------------
     *	get info from the HashJoin state 
     * ----------------
     */
    hjstate = node->hashjoinstate;
    
    /* ----------------
     * free hash table in case we end plan before all tuples are retrieved
     * ---------------
     */
    if (hjstate->hj_HashTable) {
	ExecHashTableDestroy(hjstate->hj_HashTable);
	hjstate->hj_HashTable = NULL;
    }
    
    /* ----------------
     *	Free the projection info and the scan attribute info
     *
     *  Note: we don't ExecFreeResultType(hjstate) 
     *        because the rule manager depends on the tupType
     *	      returned by ExecMain().  So for now, this
     *	      is freed at end-transaction time.  -cim 6/2/91     
     * ----------------
     */    
    ExecFreeProjectionInfo(&hjstate->jstate);
    
    /* ----------------
     * clean up subtrees 
     * ----------------
     */
    ExecEndNode(outerPlan((Plan *) node), (Plan*)node);
    ExecEndNode(innerPlan((Plan *) node), (Plan*)node);
    
    /* ----------------
     *  clean out the tuple table
     * ----------------
     */
    ExecClearTuple(hjstate->jstate.cs_ResultTupleSlot);
    ExecClearTuple(hjstate->hj_OuterTupleSlot);
    ExecClearTuple(hjstate->hj_HashTupleSlot);
    
}

/* ----------------------------------------------------------------
 *   	ExecHashJoinOuterGetTuple
 *
 *   	get the next outer tuple for hashjoin: either by
 *	executing a plan node as in the first pass, or from
 *	the tmp files for the hashjoin batches.
 * ----------------------------------------------------------------
 */

static TupleTableSlot *
ExecHashJoinOuterGetTuple(Plan *node, Plan* parent, HashJoinState *hjstate)
{
    TupleTableSlot	*slot;
    HashJoinTable	hashtable;
    int			curbatch;
    File 		*outerbatches;
    char 		*outerreadPos;
    int 		batchno;
    char 		*outerreadBuf;
    int 		outerreadBlk;
    
    hashtable = hjstate->hj_HashTable;
    curbatch = hashtable->curbatch;
    
    if (curbatch == 0) {  /* if it is the first pass */
	slot = ExecProcNode(node, parent);
	return slot;
    }
    
    /*
     * otherwise, read from the tmp files
     */
    outerbatches = hjstate->hj_OuterBatches;
    outerreadPos = hjstate->hj_OuterReadPos;
    outerreadBlk = hjstate->hj_OuterReadBlk;
    outerreadBuf = ABSADDR(hashtable->readbuf); 
    batchno = curbatch - 1;
    
    slot = ExecHashJoinGetSavedTuple(hjstate,
				     outerreadBuf,
				     outerbatches[batchno],
				     hjstate->hj_OuterTupleSlot,
				     &outerreadBlk,
				     &outerreadPos);
    
    hjstate->hj_OuterReadPos = outerreadPos;
    hjstate->hj_OuterReadBlk = outerreadBlk;
    
    return slot;
}

/* ----------------------------------------------------------------
 *   	ExecHashJoinGetSavedTuple
 *
 *   	read the next tuple from a tmp file using a certain buffer
 * ----------------------------------------------------------------
 */

static TupleTableSlot *
ExecHashJoinGetSavedTuple(HashJoinState *hjstate,
			  char *buffer,
			  File file,
			  TupleTableSlot *tupleSlot,
			  int *block,		/* return parameter */
			  char **position)	/* return parameter */
{
    char 	*bufstart;
    char 	*bufend;
    int	 	cc;
    HeapTuple 	heapTuple;
    HashJoinTable hashtable;
    
    hashtable = hjstate->hj_HashTable;
    bufend = buffer + *(long*)buffer;
    bufstart = (char*)(buffer + sizeof(long));
    if ((*position == NULL) || (*position >= bufend)) {
	if (*position == NULL)
	    (*block) = 0;
	else
	    (*block)++;
	FileSeek(file, *block * BLCKSZ, SEEK_SET);
	cc = FileRead(file, buffer, BLCKSZ);
	NDirectFileRead++;
	if (cc < 0)
	    perror("FileRead");
	if (cc == 0)  /* end of file */
	    return NULL;
	else
	    (*position) = bufstart;
    }
    heapTuple = (HeapTuple) (*position);
    (*position) = (char*)LONGALIGN(*position + heapTuple->t_len);
    
    return ExecStoreTuple(heapTuple,tupleSlot,InvalidBuffer,false);
}

/* ----------------------------------------------------------------
 *   	ExecHashJoinNewBatch
 *
 *   	switch to a new hashjoin batch
 * ----------------------------------------------------------------
 */
int
ExecHashJoinNewBatch(HashJoinState *hjstate)
{
    File 		*innerBatches;
    File 		*outerBatches;
    int 		*innerBatchSizes;
    Var 		*innerhashkey;
    HashJoinTable 	hashtable;
    int 		nbatch;
    char 		*readPos;
    int 		readBlk;
    char 		*readBuf;
    TupleTableSlot 	*slot;
    ExprContext 	*econtext;
    int 		i;
    int 		cc;
    int			newbatch;
    
    hashtable = hjstate->hj_HashTable;
    outerBatches = hjstate->hj_OuterBatches;
    innerBatches = hjstate->hj_InnerBatches;
    nbatch = hashtable->nbatch;
    newbatch = hashtable->curbatch + 1;

    /* ------------------
     *  this is the last process, so it will do the cleanup and
     *  batch-switching.
     * ------------------
     */
	if (newbatch == 1) {
	    /* 
	     * if it is end of the first pass, flush all the last pages for
	     * the batches.
	     */
	    outerBatches = hjstate->hj_OuterBatches;
	    for (i=0; i<nbatch; i++) {
		cc = FileSeek(outerBatches[i], 0L, SEEK_END);
		if (cc < 0)
		    perror("FileSeek");
		cc = FileWrite(outerBatches[i],
			       ABSADDR(hashtable->batch) + i * BLCKSZ, BLCKSZ);
		NDirectFileWrite++;
		if (cc < 0)
		    perror("FileWrite");
	    }
	}
    if (newbatch > 1) {
	/*
	 * remove the previous outer batch
	 */
	FileUnlink(outerBatches[newbatch - 2]);
    }
    /*
     * rebuild the hash table for the new inner batch
     */
	innerBatchSizes = (int*)ABSADDR(hashtable->innerbatchSizes);
    /* --------------
     *  skip over empty inner batches
     * --------------
     */
	while (newbatch <= nbatch && innerBatchSizes[newbatch - 1] == 0) {
	    FileUnlink(outerBatches[newbatch-1]);
	    FileUnlink(innerBatches[newbatch-1]);
	    newbatch++;
	}
    if (newbatch > nbatch) {
	hashtable->pcount = hashtable->nprocess;

	return newbatch;
    }
    ExecHashTableReset(hashtable, innerBatchSizes[newbatch - 1]);
    

    econtext = hjstate->jstate.cs_ExprContext;
    innerhashkey = hjstate->hj_InnerHashKey;
    readPos = NULL;
    readBlk = 0;
    readBuf = ABSADDR(hashtable->readbuf);
    
    while ((slot = ExecHashJoinGetSavedTuple(hjstate,
					     readBuf, 
					     innerBatches[newbatch-1],
					     hjstate->hj_HashTupleSlot,
					     &readBlk,
					     &readPos))
	   && ! TupIsNull(slot)) {
	econtext->ecxt_innertuple = slot;
	ExecHashTableInsert(hashtable, econtext, innerhashkey,NULL);
	/* possible bug - glass */
    }
    
    
    /* -----------------
     *  only the last process comes to this branch
     *  now all the processes have finished the build phase
     * ----------------
     */
	
    /*
     * after we build the hash table, the inner batch is no longer needed
     */
    FileUnlink(innerBatches[newbatch - 1]);
    hjstate->hj_OuterReadPos = NULL;
    hashtable->pcount = hashtable->nprocess;

    hashtable->curbatch = newbatch;
    return newbatch;
}

/* ----------------------------------------------------------------
 *   	ExecHashJoinGetBatch
 *
 *   	determine the batch number for a bucketno
 *      +----------------+-------+-------+ ... +-------+
 *	0             nbuckets                       totalbuckets
 * batch         0           1       2     ...
 * ----------------------------------------------------------------
 */
int
ExecHashJoinGetBatch(int bucketno, HashJoinTable hashtable, int nbatch)
{
    int b;
    if (bucketno < hashtable->nbuckets || nbatch == 0)
	return 0;
    
    b = (float)(bucketno - hashtable->nbuckets) /
	(float)(hashtable->totalbuckets - hashtable->nbuckets) *
	    nbatch;
    return b+1;
}

/* ----------------------------------------------------------------
 *   	ExecHashJoinSaveTuple
 *
 *   	save a tuple to a tmp file using a buffer.
 *	the first few bytes in a page is an offset to the end
 *	of the page.
 * ----------------------------------------------------------------
 */

char *
ExecHashJoinSaveTuple(HeapTuple heapTuple,
		      char *buffer,
		      File file,
		      char *position)
{
    long	*pageend;
    char	*pagestart;
    char	*pagebound;
    int		cc;
    
    pageend = (long*)buffer;
    pagestart = (char*)(buffer + sizeof(long));
    pagebound = buffer + BLCKSZ;
    if (position == NULL)
	position = pagestart;
    
    if (position + heapTuple->t_len >= pagebound) {
	cc = FileSeek(file, 0L, SEEK_END);
	if (cc < 0)
	    perror("FileSeek");
	cc = FileWrite(file, buffer, BLCKSZ);
	NDirectFileWrite++;
	if (cc < 0)
	    perror("FileWrite");
	position = pagestart;
	*pageend = 0;
    }
    memmove(position, heapTuple, heapTuple->t_len);
    position = (char*)LONGALIGN(position + heapTuple->t_len);
    *pageend = position - buffer;
    
    return position;
}
