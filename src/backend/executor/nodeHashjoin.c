/*-------------------------------------------------------------------------
 *
 * nodeHashjoin.c
 *	  Routines to handle hash join nodes
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeHashjoin.c,v 1.57.2.2 2004/09/17 18:29:10 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "optimizer/clauses.h"
#include "utils/memutils.h"


static TupleTableSlot *ExecHashJoinOuterGetTuple(PlanState *node,
						  HashJoinState *hjstate);
static TupleTableSlot *ExecHashJoinGetSavedTuple(HashJoinState *hjstate,
						  BufFile *file,
						  TupleTableSlot *tupleSlot);
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
ExecHashJoin(HashJoinState *node)
{
	EState	   *estate;
	PlanState  *outerNode;
	HashState  *hashNode;
	List	   *hjclauses;
	List	   *outerkeys;
	List	   *joinqual;
	List	   *otherqual;
	ScanDirection dir;
	TupleTableSlot *inntuple;
	ExprContext *econtext;
	ExprDoneCond isDone;
	HashJoinTable hashtable;
	HeapTuple	curtuple;
	TupleTableSlot *outerTupleSlot;
	int			i;

	/*
	 * get information from HashJoin node
	 */
	hjclauses = node->hashclauses;
	estate = node->js.ps.state;
	joinqual = node->js.joinqual;
	otherqual = node->js.ps.qual;
	hashNode = (HashState *) innerPlanState(node);
	outerNode = outerPlanState(node);
	dir = estate->es_direction;

	/*
	 * get information from HashJoin state
	 */
	hashtable = node->hj_HashTable;
	outerkeys = node->hj_OuterHashKeys;
	econtext = node->js.ps.ps_ExprContext;

	/*
	 * Check to see if we're still projecting out tuples from a previous
	 * join tuple (because there is a function-returning-set in the
	 * projection expressions).  If so, try to project another one.
	 */
	if (node->js.ps.ps_TupFromTlist)
	{
		TupleTableSlot *result;

		result = ExecProject(node->js.ps.ps_ProjInfo, &isDone);
		if (isDone == ExprMultipleResult)
			return result;
		/* Done with that source tuple... */
		node->js.ps.ps_TupFromTlist = false;
	}

	/*
	 * If we're doing an IN join, we want to return at most one row per
	 * outer tuple; so we can stop scanning the inner scan if we matched
	 * on the previous try.
	 */
	if (node->js.jointype == JOIN_IN &&
		node->hj_MatchedOuter)
		node->hj_NeedNewOuter = true;

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.  Note this can't
	 * happen until we're done projecting out tuples from a join tuple.
	 */
	ResetExprContext(econtext);

	/*
	 * if this is the first call, build the hash table for inner relation
	 */
	if (!node->hj_hashdone)
	{
		/*
		 * create the hash table
		 */
		Assert(hashtable == NULL);
		hashtable = ExecHashTableCreate((Hash *) hashNode->ps.plan,
										node->hj_HashOperators);
		node->hj_HashTable = hashtable;

		/*
		 * execute the Hash node, to build the hash table
		 */
		hashNode->hashtable = hashtable;
		(void) ExecProcNode((PlanState *) hashNode);

		/*
		 * Open temp files for outer batches, if needed. Note that file
		 * buffers are palloc'd in regular executor context.
		 */
		for (i = 0; i < hashtable->nbatch; i++)
			hashtable->outerBatchFile[i] = BufFileCreateTemp(false);

		node->hj_hashdone = true;
	}

	/*
	 * Now get an outer tuple and probe into the hash table for matches
	 */
	outerTupleSlot = node->js.ps.ps_OuterTupleSlot;

	for (;;)
	{
		/*
		 * If we don't have an outer tuple, get the next one
		 */
		if (node->hj_NeedNewOuter)
		{
			outerTupleSlot = ExecHashJoinOuterGetTuple(outerNode,
													   node);
			if (TupIsNull(outerTupleSlot))
			{
				/* end of join */
				return NULL;
			}

			node->js.ps.ps_OuterTupleSlot = outerTupleSlot;
			econtext->ecxt_outertuple = outerTupleSlot;
			node->hj_NeedNewOuter = false;
			node->hj_MatchedOuter = false;

			/*
			 * now we have an outer tuple, find the corresponding bucket
			 * for this tuple from the hash table
			 */
			node->hj_CurBucketNo = ExecHashGetBucket(hashtable, econtext,
													 outerkeys);
			node->hj_CurTuple = NULL;

			/*
			 * Now we've got an outer tuple and the corresponding hash
			 * bucket, but this tuple may not belong to the current batch.
			 * This need only be checked in the first pass.
			 */
			if (hashtable->curbatch == 0)
			{
				int			batchno = ExecHashGetBatch(node->hj_CurBucketNo,
													   hashtable);

				if (batchno >= 0)
				{
					/*
					 * Need to postpone this outer tuple to a later batch.
					 * Save it in the corresponding outer-batch file.
					 */
					hashtable->outerBatchSize[batchno]++;
					ExecHashJoinSaveTuple(outerTupleSlot->val,
									 hashtable->outerBatchFile[batchno]);
					node->hj_NeedNewOuter = true;
					continue;	/* loop around for a new outer tuple */
				}
			}
		}

		/*
		 * OK, scan the selected hash bucket for matches
		 */
		for (;;)
		{
			curtuple = ExecScanHashBucket(node,
										  hjclauses,
										  econtext);
			if (curtuple == NULL)
				break;			/* out of matches */

			/*
			 * we've got a match, but still need to test non-hashed quals
			 */
			inntuple = ExecStoreTuple(curtuple,
									  node->hj_HashTupleSlot,
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
				node->hj_MatchedOuter = true;

				if (otherqual == NIL || ExecQual(otherqual, econtext, false))
				{
					TupleTableSlot *result;

					result = ExecProject(node->js.ps.ps_ProjInfo, &isDone);

					if (isDone != ExprEndResult)
					{
						node->js.ps.ps_TupFromTlist =
							(isDone == ExprMultipleResult);
						return result;
					}
				}

				/*
				 * If we didn't return a tuple, may need to set
				 * NeedNewOuter
				 */
				if (node->js.jointype == JOIN_IN)
				{
					node->hj_NeedNewOuter = true;
					break;		/* out of loop over hash bucket */
				}
			}
		}

		/*
		 * Now the current outer tuple has run out of matches, so check
		 * whether to emit a dummy outer-join tuple. If not, loop around
		 * to get a new outer tuple.
		 */
		node->hj_NeedNewOuter = true;

		if (!node->hj_MatchedOuter &&
			node->js.jointype == JOIN_LEFT)
		{
			/*
			 * We are doing an outer join and there were no join matches
			 * for this outer tuple.  Generate a fake join tuple with
			 * nulls for the inner tuple, and return it if it passes the
			 * non-join quals.
			 */
			econtext->ecxt_innertuple = node->hj_NullInnerTupleSlot;

			if (ExecQual(otherqual, econtext, false))
			{
				/*
				 * qualification was satisfied so we project and return
				 * the slot containing the result tuple using
				 * ExecProject().
				 */
				TupleTableSlot *result;

				result = ExecProject(node->js.ps.ps_ProjInfo, &isDone);

				if (isDone != ExprEndResult)
				{
					node->js.ps.ps_TupFromTlist =
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
HashJoinState *
ExecInitHashJoin(HashJoin *node, EState *estate)
{
	HashJoinState *hjstate;
	Plan	   *outerNode;
	Hash	   *hashNode;
	List	   *hclauses;
	List	   *hoperators;
	List	   *hcl;

	/*
	 * create state structure
	 */
	hjstate = makeNode(HashJoinState);
	hjstate->js.ps.plan = (Plan *) node;
	hjstate->js.ps.state = estate;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &hjstate->js.ps);

	/*
	 * initialize child expressions
	 */
	hjstate->js.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->join.plan.targetlist,
					 (PlanState *) hjstate);
	hjstate->js.ps.qual = (List *)
		ExecInitExpr((Expr *) node->join.plan.qual,
					 (PlanState *) hjstate);
	hjstate->js.jointype = node->join.jointype;
	hjstate->js.joinqual = (List *)
		ExecInitExpr((Expr *) node->join.joinqual,
					 (PlanState *) hjstate);
	hjstate->hashclauses = (List *)
		ExecInitExpr((Expr *) node->hashclauses,
					 (PlanState *) hjstate);

	/*
	 * initialize child nodes
	 */
	outerNode = outerPlan(node);
	hashNode = (Hash *) innerPlan(node);

	outerPlanState(hjstate) = ExecInitNode(outerNode, estate);
	innerPlanState(hjstate) = ExecInitNode((Plan *) hashNode, estate);

#define HASHJOIN_NSLOTS 3

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &hjstate->js.ps);
	hjstate->hj_OuterTupleSlot = ExecInitExtraTupleSlot(estate);

	switch (node->join.jointype)
	{
		case JOIN_INNER:
		case JOIN_IN:
			break;
		case JOIN_LEFT:
			hjstate->hj_NullInnerTupleSlot =
				ExecInitNullTupleSlot(estate,
							 ExecGetResultType(innerPlanState(hjstate)));
			break;
		default:
			elog(ERROR, "unrecognized join type: %d",
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
		HashState  *hashstate = (HashState *) innerPlanState(hjstate);
		TupleTableSlot *slot = hashstate->ps.ps_ResultTupleSlot;

		hjstate->hj_HashTupleSlot = slot;
	}

	/*
	 * initialize tuple type and projection info
	 */
	ExecAssignResultTypeFromTL(&hjstate->js.ps);
	ExecAssignProjectionInfo(&hjstate->js.ps);

	ExecSetSlotDescriptor(hjstate->hj_OuterTupleSlot,
						  ExecGetResultType(outerPlanState(hjstate)),
						  false);

	/*
	 * initialize hash-specific info
	 */

	hjstate->hj_hashdone = false;
	hjstate->hj_HashTable = (HashJoinTable) NULL;

	hjstate->hj_CurBucketNo = 0;
	hjstate->hj_CurTuple = (HashJoinTuple) NULL;

	/*
	 * The planner already made a list of the inner hashkeys for us, but
	 * we also need a list of the outer hashkeys, as well as a list of the
	 * hash operator OIDs.	Both lists of exprs must then be prepared for
	 * execution.
	 */
	hjstate->hj_InnerHashKeys = (List *)
		ExecInitExpr((Expr *) hashNode->hashkeys,
					 (PlanState *) hjstate);
	((HashState *) innerPlanState(hjstate))->hashkeys =
		hjstate->hj_InnerHashKeys;

	hclauses = NIL;
	hoperators = NIL;
	foreach(hcl, node->hashclauses)
	{
		OpExpr	   *hclause = (OpExpr *) lfirst(hcl);

		Assert(IsA(hclause, OpExpr));
		hclauses = lappend(hclauses, get_leftop((Expr *) hclause));
		hoperators = lappendo(hoperators, hclause->opno);
	}
	hjstate->hj_OuterHashKeys = (List *)
		ExecInitExpr((Expr *) hclauses,
					 (PlanState *) hjstate);
	hjstate->hj_HashOperators = hoperators;

	hjstate->js.ps.ps_OuterTupleSlot = NULL;
	hjstate->js.ps.ps_TupFromTlist = false;
	hjstate->hj_NeedNewOuter = true;
	hjstate->hj_MatchedOuter = false;

	return hjstate;
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
ExecEndHashJoin(HashJoinState *node)
{
	/*
	 * Free hash table
	 */
	if (node->hj_HashTable)
	{
		ExecHashTableDestroy(node->hj_HashTable);
		node->hj_HashTable = NULL;
	}

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->js.ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->js.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->hj_OuterTupleSlot);
	ExecClearTuple(node->hj_HashTupleSlot);

	/*
	 * clean up subtrees
	 */
	ExecEndNode(outerPlanState(node));
	ExecEndNode(innerPlanState(node));
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
ExecHashJoinOuterGetTuple(PlanState *node, HashJoinState *hjstate)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	int			curbatch = hashtable->curbatch;
	TupleTableSlot *slot;

	if (curbatch == 0)
	{							/* if it is the first pass */
		slot = ExecProcNode(node);
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
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from hash-join temporary file: %m")));
	heapTuple = palloc(HEAPTUPLESIZE + htup.t_len);
	memcpy((char *) heapTuple, (char *) &htup, sizeof(HeapTupleData));
	heapTuple->t_datamcxt = CurrentMemoryContext;
	heapTuple->t_data = (HeapTupleHeader)
		((char *) heapTuple + HEAPTUPLESIZE);
	nread = BufFileRead(file, (void *) heapTuple->t_data, htup.t_len);
	if (nread != (size_t) htup.t_len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from hash-join temporary file: %m")));
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
	List	   *innerhashkeys;

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
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rewind hash-join temporary file: %m")));

	innerFile = hashtable->innerBatchFile[newbatch - 1];

	if (BufFileSeek(innerFile, 0, 0L, SEEK_SET))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rewind hash-join temporary file: %m")));

	/*
	 * Reload the hash table with the new inner batch
	 */
	ExecHashTableReset(hashtable, innerBatchSize[newbatch - 1]);

	econtext = hjstate->js.ps.ps_ExprContext;
	innerhashkeys = hjstate->hj_InnerHashKeys;

	while ((slot = ExecHashJoinGetSavedTuple(hjstate,
											 innerFile,
											 hjstate->hj_HashTupleSlot))
		   && !TupIsNull(slot))
	{
		econtext->ecxt_innertuple = slot;
		ExecHashTableInsert(hashtable, econtext, innerhashkeys);
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
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to hash-join temporary file: %m")));
	written = BufFileWrite(file, (void *) heapTuple->t_data, heapTuple->t_len);
	if (written != (size_t) heapTuple->t_len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to hash-join temporary file: %m")));
}

void
ExecReScanHashJoin(HashJoinState *node, ExprContext *exprCtxt)
{
	/*
	 * If we haven't yet built the hash table then we can just return;
	 * nothing done yet, so nothing to undo.
	 */
	if (!node->hj_hashdone)
		return;
	Assert(node->hj_HashTable != NULL);

	/*
	 * In a multi-batch join, we currently have to do rescans the hard
	 * way, primarily because batch temp files may have already been
	 * released. But if it's a single-batch join, and there is no
	 * parameter change for the inner subnode, then we can just re-use the
	 * existing hash table without rebuilding it.
	 */
	if (node->hj_HashTable->nbatch == 0 &&
		((PlanState *) node)->righttree->chgParam == NULL)
	{
		/* okay to reuse the hash table; needn't rescan inner, either */
	}
	else
	{
		/* must destroy and rebuild hash table */
		node->hj_hashdone = false;
		ExecHashTableDestroy(node->hj_HashTable);
		node->hj_HashTable = NULL;

		/*
		 * if chgParam of subnode is not null then plan will be re-scanned
		 * by first ExecProcNode.
		 */
		if (((PlanState *) node)->righttree->chgParam == NULL)
			ExecReScan(((PlanState *) node)->righttree, exprCtxt);
	}

	/* Always reset intra-tuple state */
	node->hj_CurBucketNo = 0;
	node->hj_CurTuple = (HashJoinTuple) NULL;

	node->js.ps.ps_OuterTupleSlot = (TupleTableSlot *) NULL;
	node->js.ps.ps_TupFromTlist = false;
	node->hj_NeedNewOuter = true;
	node->hj_MatchedOuter = false;

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((PlanState *) node)->lefttree->chgParam == NULL)
		ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
}
