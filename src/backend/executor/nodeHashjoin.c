/*-------------------------------------------------------------------------
 *
 * nodeHashjoin.c
 *	  Routines to handle hash join nodes
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeHashjoin.c,v 1.41.2.1 2004/09/17 18:29:24 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "optimizer/clauses.h"
#include "utils/memutils.h"


static TupleTableSlot *ExecHashJoinOuterGetTuple(Plan *node, Plan *parent,
						  HashJoinState *hjstate);
static TupleTableSlot *ExecHashJoinGetSavedTuple(HashJoinState *hjstate,
						  BufFile *file,
						  TupleTableSlot *tupleSlot);
static int	ExecHashJoinGetBatch(int bucketno, HashJoinTable hashtable);
static int	ExecHashJoinNewBatch(HashJoinState *hjstate);


/* ----------------------------------------------------------------
 *		ExecHashJoin
 *
 *		This function implements the Hybrid Hashjoin algorithm.
 *		recursive partitioning remains to be added.
 *		Note: the relation we build hash table on is the inner
 *			  the other one is outer.
 * ----------------------------------------------------------------
 */
TupleTableSlot *				/* return: a tuple or NULL */
ExecHashJoin(HashJoin *node)
{
	HashJoinState *hjstate;
	EState	   *estate;
	Plan	   *outerNode;
	Hash	   *hashNode;
	List	   *hjclauses;
	Expr	   *clause;
	List	   *joinqual;
	List	   *otherqual;
	ScanDirection dir;
	TupleTableSlot *inntuple;
	Node	   *outerVar;
	ExprContext *econtext;
	ExprDoneCond isDone;
	HashJoinTable hashtable;
	HeapTuple	curtuple;
	TupleTableSlot *outerTupleSlot;
	TupleTableSlot *innerTupleSlot;
	int			i;
	bool		hashPhaseDone;

	/*
	 * get information from HashJoin node
	 */
	hjstate = node->hashjoinstate;
	hjclauses = node->hashclauses;
	clause = lfirst(hjclauses);
	estate = node->join.plan.state;
	joinqual = node->join.joinqual;
	otherqual = node->join.plan.qual;
	hashNode = (Hash *) innerPlan(node);
	outerNode = outerPlan(node);
	hashPhaseDone = hjstate->hj_hashdone;
	dir = estate->es_direction;

	/*
	 * get information from HashJoin state
	 */
	hashtable = hjstate->hj_HashTable;
	econtext = hjstate->jstate.cs_ExprContext;

	/*
	 * Check to see if we're still projecting out tuples from a previous
	 * join tuple (because there is a function-returning-set in the
	 * projection expressions).  If so, try to project another one.
	 */
	if (hjstate->jstate.cs_TupFromTlist)
	{
		TupleTableSlot *result;

		result = ExecProject(hjstate->jstate.cs_ProjInfo, &isDone);
		if (isDone == ExprMultipleResult)
			return result;
		/* Done with that source tuple... */
		hjstate->jstate.cs_TupFromTlist = false;
	}

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.  Note this can't
	 * happen until we're done projecting out tuples from a join tuple.
	 */
	ResetExprContext(econtext);

	/*
	 * if this is the first call, build the hash table for inner relation
	 */
	if (!hashPhaseDone)
	{							/* if the hash phase not completed */
		if (hashtable == NULL)
		{						/* if the hash table has not been created */

			/*
			 * create the hash table
			 */
			hashtable = ExecHashTableCreate(hashNode);
			hjstate->hj_HashTable = hashtable;
			hjstate->hj_InnerHashKey = hashNode->hashkey;

			/*
			 * execute the Hash node, to build the hash table
			 */
			hashNode->hashstate->hashtable = hashtable;
			innerTupleSlot = ExecProcNode((Plan *) hashNode, (Plan *) node);
		}
		hjstate->hj_hashdone = true;

		/*
		 * Open temp files for outer batches, if needed. Note that file
		 * buffers are palloc'd in regular executor context.
		 */
		for (i = 0; i < hashtable->nbatch; i++)
			hashtable->outerBatchFile[i] = BufFileCreateTemp();
	}
	else if (hashtable == NULL)
		return NULL;

	/*
	 * Now get an outer tuple and probe into the hash table for matches
	 */
	outerTupleSlot = hjstate->jstate.cs_OuterTupleSlot;
	outerVar = (Node *) get_leftop(clause);

	for (;;)
	{
		/*
		 * If we don't have an outer tuple, get the next one
		 */
		if (hjstate->hj_NeedNewOuter)
		{
			outerTupleSlot = ExecHashJoinOuterGetTuple(outerNode,
													   (Plan *) node,
													   hjstate);
			if (TupIsNull(outerTupleSlot))
			{
				/*
				 * when the last batch runs out, clean up and exit
				 */
				ExecHashTableDestroy(hashtable);
				hjstate->hj_HashTable = NULL;
				return NULL;
			}

			hjstate->jstate.cs_OuterTupleSlot = outerTupleSlot;
			econtext->ecxt_outertuple = outerTupleSlot;
			hjstate->hj_NeedNewOuter = false;
			hjstate->hj_MatchedOuter = false;

			/*
			 * now we have an outer tuple, find the corresponding bucket
			 * for this tuple from the hash table
			 */
			hjstate->hj_CurBucketNo = ExecHashGetBucket(hashtable, econtext,
														outerVar);
			hjstate->hj_CurTuple = NULL;

			/*
			 * Now we've got an outer tuple and the corresponding hash
			 * bucket, but this tuple may not belong to the current batch.
			 * This need only be checked in the first pass.
			 */
			if (hashtable->curbatch == 0)
			{
				int			batch = ExecHashJoinGetBatch(hjstate->hj_CurBucketNo,
														 hashtable);

				if (batch > 0)
				{
					/*
					 * Need to postpone this outer tuple to a later batch.
					 * Save it in the corresponding outer-batch file.
					 */
					int			batchno = batch - 1;

					hashtable->outerBatchSize[batchno]++;
					ExecHashJoinSaveTuple(outerTupleSlot->val,
									 hashtable->outerBatchFile[batchno]);
					hjstate->hj_NeedNewOuter = true;
					continue;	/* loop around for a new outer tuple */
				}
			}
		}

		/*
		 * OK, scan the selected hash bucket for matches
		 */
		for (;;)
		{
			curtuple = ExecScanHashBucket(hjstate,
										  hjclauses,
										  econtext);
			if (curtuple == NULL)
				break;			/* out of matches */

			/*
			 * we've got a match, but still need to test non-hashed quals
			 */
			inntuple = ExecStoreTuple(curtuple,
									  hjstate->hj_HashTupleSlot,
									  InvalidBuffer,
									  false);	/* don't pfree this tuple */
			econtext->ecxt_innertuple = inntuple;

			/* reset temp memory each time to avoid leaks from qual expr */
			ResetExprContext(econtext);

			/*
			 * if we pass the qual, then save state for next call and have
			 * ExecProject form the projection, store it in the tuple
			 * table, and return the slot.
			 *
			 * Only the joinquals determine MatchedOuter status, but all
			 * quals must pass to actually return the tuple.
			 */
			if (ExecQual(joinqual, econtext, false))
			{
				hjstate->hj_MatchedOuter = true;

				if (otherqual == NIL || ExecQual(otherqual, econtext, false))
				{
					TupleTableSlot *result;

					result = ExecProject(hjstate->jstate.cs_ProjInfo, &isDone);

					if (isDone != ExprEndResult)
					{
						hjstate->jstate.cs_TupFromTlist =
							(isDone == ExprMultipleResult);
						return result;
					}
				}
			}
		}

		/*
		 * Now the current outer tuple has run out of matches, so check
		 * whether to emit a dummy outer-join tuple. If not, loop around
		 * to get a new outer tuple.
		 */
		hjstate->hj_NeedNewOuter = true;

		if (!hjstate->hj_MatchedOuter &&
			node->join.jointype == JOIN_LEFT)
		{
			/*
			 * We are doing an outer join and there were no join matches
			 * for this outer tuple.  Generate a fake join tuple with
			 * nulls for the inner tuple, and return it if it passes the
			 * non-join quals.
			 */
			econtext->ecxt_innertuple = hjstate->hj_NullInnerTupleSlot;

			if (ExecQual(otherqual, econtext, false))
			{
				/*
				 * qualification was satisfied so we project and return
				 * the slot containing the result tuple using
				 * ExecProject().
				 */
				TupleTableSlot *result;

				result = ExecProject(hjstate->jstate.cs_ProjInfo, &isDone);

				if (isDone != ExprEndResult)
				{
					hjstate->jstate.cs_TupFromTlist =
						(isDone == ExprMultipleResult);
					return result;
				}
			}
		}
	}
}

/* ----------------------------------------------------------------
 *		ExecInitHashJoin
 *
 *		Init routine for HashJoin node.
 * ----------------------------------------------------------------
 */
bool							/* return: initialization status */
ExecInitHashJoin(HashJoin *node, EState *estate, Plan *parent)
{
	HashJoinState *hjstate;
	Plan	   *outerNode;
	Hash	   *hashNode;

	/*
	 * assign the node's execution state
	 */
	node->join.plan.state = estate;

	/*
	 * create state structure
	 */
	hjstate = makeNode(HashJoinState);
	node->hashjoinstate = hjstate;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &hjstate->jstate);

	/*
	 * initializes child nodes
	 */
	outerNode = outerPlan((Plan *) node);
	hashNode = (Hash *) innerPlan((Plan *) node);

	ExecInitNode(outerNode, estate, (Plan *) node);
	ExecInitNode((Plan *) hashNode, estate, (Plan *) node);

#define HASHJOIN_NSLOTS 3

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &hjstate->jstate);
	hjstate->hj_OuterTupleSlot = ExecInitExtraTupleSlot(estate);

	switch (node->join.jointype)
	{
		case JOIN_INNER:
			break;
		case JOIN_LEFT:
			hjstate->hj_NullInnerTupleSlot =
				ExecInitNullTupleSlot(estate,
									  ExecGetTupType((Plan *) hashNode));
			break;
		default:
			elog(ERROR, "ExecInitHashJoin: unsupported join type %d",
				 (int) node->join.jointype);
	}

	/*
	 * now for some voodoo.  our temporary tuple slot is actually the
	 * result tuple slot of the Hash node (which is our inner plan).  we
	 * do this because Hash nodes don't return tuples via ExecProcNode()
	 * -- instead the hash join node uses ExecScanHashBucket() to get at
	 * the contents of the hash table.	-cim 6/9/91
	 */
	{
		HashState  *hashstate = hashNode->hashstate;
		TupleTableSlot *slot = hashstate->cstate.cs_ResultTupleSlot;

		hjstate->hj_HashTupleSlot = slot;
	}

	/*
	 * initialize tuple type and projection info
	 */
	ExecAssignResultTypeFromTL((Plan *) node, &hjstate->jstate);
	ExecAssignProjectionInfo((Plan *) node, &hjstate->jstate);

	ExecSetSlotDescriptor(hjstate->hj_OuterTupleSlot,
						  ExecGetTupType(outerNode),
						  false);

	/*
	 * initialize hash-specific info
	 */

	hjstate->hj_hashdone = false;

	hjstate->hj_HashTable = (HashJoinTable) NULL;
	hjstate->hj_CurBucketNo = 0;
	hjstate->hj_CurTuple = (HashJoinTuple) NULL;
	hjstate->hj_InnerHashKey = (Node *) NULL;

	hjstate->jstate.cs_OuterTupleSlot = NULL;
	hjstate->jstate.cs_TupFromTlist = false;
	hjstate->hj_NeedNewOuter = true;
	hjstate->hj_MatchedOuter = false;

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
 *		ExecEndHashJoin
 *
 *		clean up routine for HashJoin node
 * ----------------------------------------------------------------
 */
void
ExecEndHashJoin(HashJoin *node)
{
	HashJoinState *hjstate;

	/*
	 * get info from the HashJoin state
	 */
	hjstate = node->hashjoinstate;

	/*
	 * free hash table in case we end plan before all tuples are retrieved
	 */
	if (hjstate->hj_HashTable)
	{
		ExecHashTableDestroy(hjstate->hj_HashTable);
		hjstate->hj_HashTable = NULL;
	}

	/*
	 * Free the projection info and the scan attribute info
	 *
	 * Note: we don't ExecFreeResultType(hjstate) because the rule manager
	 * depends on the tupType returned by ExecMain().  So for now, this is
	 * freed at end-transaction time.  -cim 6/2/91
	 */
	ExecFreeProjectionInfo(&hjstate->jstate);
	ExecFreeExprContext(&hjstate->jstate);

	/*
	 * clean up subtrees
	 */
	ExecEndNode(outerPlan((Plan *) node), (Plan *) node);
	ExecEndNode(innerPlan((Plan *) node), (Plan *) node);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(hjstate->jstate.cs_ResultTupleSlot);
	ExecClearTuple(hjstate->hj_OuterTupleSlot);
	ExecClearTuple(hjstate->hj_HashTupleSlot);

}

/* ----------------------------------------------------------------
 *		ExecHashJoinOuterGetTuple
 *
 *		get the next outer tuple for hashjoin: either by
 *		executing a plan node as in the first pass, or from
 *		the tmp files for the hashjoin batches.
 * ----------------------------------------------------------------
 */

static TupleTableSlot *
ExecHashJoinOuterGetTuple(Plan *node, Plan *parent, HashJoinState *hjstate)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	int			curbatch = hashtable->curbatch;
	TupleTableSlot *slot;

	if (curbatch == 0)
	{							/* if it is the first pass */
		slot = ExecProcNode(node, parent);
		if (!TupIsNull(slot))
			return slot;

		/*
		 * We have just reached the end of the first pass. Try to switch
		 * to a saved batch.
		 */
		curbatch = ExecHashJoinNewBatch(hjstate);
	}

	/*
	 * Try to read from a temp file. Loop allows us to advance to new
	 * batch as needed.
	 */
	while (curbatch <= hashtable->nbatch)
	{
		slot = ExecHashJoinGetSavedTuple(hjstate,
								 hashtable->outerBatchFile[curbatch - 1],
										 hjstate->hj_OuterTupleSlot);
		if (!TupIsNull(slot))
			return slot;
		curbatch = ExecHashJoinNewBatch(hjstate);
	}

	/* Out of batches... */
	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecHashJoinGetSavedTuple
 *
 *		read the next tuple from a tmp file
 * ----------------------------------------------------------------
 */

static TupleTableSlot *
ExecHashJoinGetSavedTuple(HashJoinState *hjstate,
						  BufFile *file,
						  TupleTableSlot *tupleSlot)
{
	HeapTupleData htup;
	size_t		nread;
	HeapTuple	heapTuple;

	nread = BufFileRead(file, (void *) &htup, sizeof(HeapTupleData));
	if (nread == 0)
		return NULL;			/* end of file */
	if (nread != sizeof(HeapTupleData))
		elog(ERROR, "Read from hashjoin temp file failed");
	heapTuple = palloc(HEAPTUPLESIZE + htup.t_len);
	memcpy((char *) heapTuple, (char *) &htup, sizeof(HeapTupleData));
	heapTuple->t_datamcxt = CurrentMemoryContext;
	heapTuple->t_data = (HeapTupleHeader)
		((char *) heapTuple + HEAPTUPLESIZE);
	nread = BufFileRead(file, (void *) heapTuple->t_data, htup.t_len);
	if (nread != (size_t) htup.t_len)
		elog(ERROR, "Read from hashjoin temp file failed");
	return ExecStoreTuple(heapTuple, tupleSlot, InvalidBuffer, true);
}

/* ----------------------------------------------------------------
 *		ExecHashJoinNewBatch
 *
 *		switch to a new hashjoin batch
 * ----------------------------------------------------------------
 */
static int
ExecHashJoinNewBatch(HashJoinState *hjstate)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	int			nbatch = hashtable->nbatch;
	int			newbatch = hashtable->curbatch + 1;
	long	   *innerBatchSize = hashtable->innerBatchSize;
	long	   *outerBatchSize = hashtable->outerBatchSize;
	BufFile    *innerFile;
	TupleTableSlot *slot;
	ExprContext *econtext;
	Node	   *innerhashkey;

	if (newbatch > 1)
	{
		/*
		 * We no longer need the previous outer batch file; close it right
		 * away to free disk space.
		 */
		BufFileClose(hashtable->outerBatchFile[newbatch - 2]);
		hashtable->outerBatchFile[newbatch - 2] = NULL;
	}

	/*
	 * Normally we can skip over any batches that are empty on either side
	 * --- but for JOIN_LEFT, can only skip when left side is empty.
	 * Release associated temp files right away.
	 */
	while (newbatch <= nbatch &&
		   (outerBatchSize[newbatch - 1] == 0L ||
			(innerBatchSize[newbatch - 1] == 0L &&
			 hjstate->js.jointype != JOIN_LEFT)))
	{
		BufFileClose(hashtable->innerBatchFile[newbatch - 1]);
		hashtable->innerBatchFile[newbatch - 1] = NULL;
		BufFileClose(hashtable->outerBatchFile[newbatch - 1]);
		hashtable->outerBatchFile[newbatch - 1] = NULL;
		newbatch++;
	}

	if (newbatch > nbatch)
		return newbatch;		/* no more batches */

	/*
	 * Rewind inner and outer batch files for this batch, so that we can
	 * start reading them.
	 */
	if (BufFileSeek(hashtable->outerBatchFile[newbatch - 1], 0, 0L, SEEK_SET))
		elog(ERROR, "Failed to rewind hash temp file");

	innerFile = hashtable->innerBatchFile[newbatch - 1];

	if (BufFileSeek(innerFile, 0, 0L, SEEK_SET))
		elog(ERROR, "Failed to rewind hash temp file");

	/*
	 * Reload the hash table with the new inner batch
	 */
	ExecHashTableReset(hashtable, innerBatchSize[newbatch - 1]);

	econtext = hjstate->jstate.cs_ExprContext;
	innerhashkey = hjstate->hj_InnerHashKey;

	while ((slot = ExecHashJoinGetSavedTuple(hjstate,
											 innerFile,
											 hjstate->hj_HashTupleSlot))
		   && !TupIsNull(slot))
	{
		econtext->ecxt_innertuple = slot;
		ExecHashTableInsert(hashtable, econtext, innerhashkey);
	}

	/*
	 * after we build the hash table, the inner batch file is no longer
	 * needed
	 */
	BufFileClose(innerFile);
	hashtable->innerBatchFile[newbatch - 1] = NULL;

	hashtable->curbatch = newbatch;
	return newbatch;
}

/* ----------------------------------------------------------------
 *		ExecHashJoinGetBatch
 *
 *		determine the batch number for a bucketno
 *		+----------------+-------+-------+ ... +-------+
 *		0			  nbuckets						 totalbuckets
 * batch		 0			 1		 2	   ...
 * ----------------------------------------------------------------
 */
static int
ExecHashJoinGetBatch(int bucketno, HashJoinTable hashtable)
{
	int			b;

	if (bucketno < hashtable->nbuckets || hashtable->nbatch == 0)
		return 0;

	b = (hashtable->nbatch * (bucketno - hashtable->nbuckets)) /
		(hashtable->totalbuckets - hashtable->nbuckets);
	return b + 1;
}

/* ----------------------------------------------------------------
 *		ExecHashJoinSaveTuple
 *
 *		save a tuple to a tmp file.
 *
 * The data recorded in the file for each tuple is an image of its
 * HeapTupleData (with meaningless t_data pointer) followed by the
 * HeapTupleHeader and tuple data.
 * ----------------------------------------------------------------
 */

void
ExecHashJoinSaveTuple(HeapTuple heapTuple,
					  BufFile *file)
{
	size_t		written;

	written = BufFileWrite(file, (void *) heapTuple, sizeof(HeapTupleData));
	if (written != sizeof(HeapTupleData))
		elog(ERROR, "Write to hashjoin temp file failed");
	written = BufFileWrite(file, (void *) heapTuple->t_data, heapTuple->t_len);
	if (written != (size_t) heapTuple->t_len)
		elog(ERROR, "Write to hashjoin temp file failed");
}

void
ExecReScanHashJoin(HashJoin *node, ExprContext *exprCtxt, Plan *parent)
{
	HashJoinState *hjstate = node->hashjoinstate;

	if (!hjstate->hj_hashdone)
		return;

	hjstate->hj_hashdone = false;

	/*
	 * Unfortunately, currently we have to destroy hashtable in all
	 * cases...
	 */
	if (hjstate->hj_HashTable)
	{
		ExecHashTableDestroy(hjstate->hj_HashTable);
		hjstate->hj_HashTable = NULL;
	}

	hjstate->hj_CurBucketNo = 0;
	hjstate->hj_CurTuple = (HashJoinTuple) NULL;
	hjstate->hj_InnerHashKey = (Node *) NULL;

	hjstate->jstate.cs_OuterTupleSlot = (TupleTableSlot *) NULL;
	hjstate->jstate.cs_TupFromTlist = false;
	hjstate->hj_NeedNewOuter = true;
	hjstate->hj_MatchedOuter = false;

	/*
	 * if chgParam of subnodes is not null then plans will be re-scanned
	 * by first ExecProcNode.
	 */
	if (((Plan *) node)->lefttree->chgParam == NULL)
		ExecReScan(((Plan *) node)->lefttree, exprCtxt, (Plan *) node);
	if (((Plan *) node)->righttree->chgParam == NULL)
		ExecReScan(((Plan *) node)->righttree, exprCtxt, (Plan *) node);
}
