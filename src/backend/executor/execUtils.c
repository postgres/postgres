/*-------------------------------------------------------------------------
 *
 * execUtils.c
 *	  miscellaneous executor utility routines
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execUtils.c,v 1.106.2.2 2004/01/22 02:23:35 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		CreateExecutorState		Create/delete executor working state
 *		FreeExecutorState
 *		CreateExprContext
 *		FreeExprContext
 *		ReScanExprContext
 *
 *		ExecAssignExprContext	Common code for plan node init routines.
 *		ExecAssignResultType
 *		etc
 *
 *		ExecOpenIndices			\
 *		ExecCloseIndices		 | referenced by InitPlan, EndPlan,
 *		ExecInsertIndexTuples	/  ExecInsert, ExecUpdate
 *
 *		RegisterExprContextCallback    Register function shutdown callback
 *		UnregisterExprContextCallback  Deregister function shutdown callback
 *
 *	 NOTES
 *		This file has traditionally been the place to stick misc.
 *		executor support stuff that doesn't really go anyplace else.
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/index.h"
#include "catalog/catalog.h"
#include "catalog/pg_index.h"
#include "executor/execdebug.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/syscache.h"


/* ----------------------------------------------------------------
 *		global counters for number of tuples processed, retrieved,
 *		appended, replaced, deleted.
 * ----------------------------------------------------------------
 */
int			NTupleProcessed;
int			NTupleRetrieved;
int			NTupleReplaced;
int			NTupleAppended;
int			NTupleDeleted;
int			NIndexTupleInserted;
extern int	NIndexTupleProcessed;		/* have to be defined in the
										 * access method level so that the
										 * cinterface.a will link ok. */


static void ShutdownExprContext(ExprContext *econtext);


/* ----------------------------------------------------------------
 *						statistic functions
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ResetTupleCount
 * ----------------------------------------------------------------
 */
#ifdef NOT_USED
void
ResetTupleCount(void)
{
	NTupleProcessed = 0;
	NTupleRetrieved = 0;
	NTupleAppended = 0;
	NTupleDeleted = 0;
	NTupleReplaced = 0;
	NIndexTupleProcessed = 0;
}
#endif

/* ----------------------------------------------------------------
 *		PrintTupleCount
 * ----------------------------------------------------------------
 */
#ifdef NOT_USED
void
DisplayTupleCount(FILE *statfp)
{
	if (NTupleProcessed > 0)
		fprintf(statfp, "!\t%d tuple%s processed, ", NTupleProcessed,
				(NTupleProcessed == 1) ? "" : "s");
	else
	{
		fprintf(statfp, "!\tno tuples processed.\n");
		return;
	}
	if (NIndexTupleProcessed > 0)
		fprintf(statfp, "%d indextuple%s processed, ", NIndexTupleProcessed,
				(NIndexTupleProcessed == 1) ? "" : "s");
	if (NIndexTupleInserted > 0)
		fprintf(statfp, "%d indextuple%s inserted, ", NIndexTupleInserted,
				(NIndexTupleInserted == 1) ? "" : "s");
	if (NTupleRetrieved > 0)
		fprintf(statfp, "%d tuple%s retrieved. ", NTupleRetrieved,
				(NTupleRetrieved == 1) ? "" : "s");
	if (NTupleAppended > 0)
		fprintf(statfp, "%d tuple%s appended. ", NTupleAppended,
				(NTupleAppended == 1) ? "" : "s");
	if (NTupleDeleted > 0)
		fprintf(statfp, "%d tuple%s deleted. ", NTupleDeleted,
				(NTupleDeleted == 1) ? "" : "s");
	if (NTupleReplaced > 0)
		fprintf(statfp, "%d tuple%s replaced. ", NTupleReplaced,
				(NTupleReplaced == 1) ? "" : "s");
	fprintf(statfp, "\n");
}
#endif


/* ----------------------------------------------------------------
 *				 Executor state and memory management functions
 * ----------------------------------------------------------------
 */

/* ----------------
 *		CreateExecutorState
 *
 *		Create and initialize an EState node, which is the root of
 *		working storage for an entire Executor invocation.
 *
 * Principally, this creates the per-query memory context that will be
 * used to hold all working data that lives till the end of the query.
 * Note that the per-query context will become a child of the caller's
 * CurrentMemoryContext.
 * ----------------
 */
EState *
CreateExecutorState(void)
{
	EState	   *estate;
	MemoryContext qcontext;
	MemoryContext oldcontext;

	/*
	 * Create the per-query context for this Executor run.
	 */
	qcontext = AllocSetContextCreate(CurrentMemoryContext,
									 "ExecutorState",
									 ALLOCSET_DEFAULT_MINSIZE,
									 ALLOCSET_DEFAULT_INITSIZE,
									 ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * Make the EState node within the per-query context.  This way, we
	 * don't need a separate pfree() operation for it at shutdown.
	 */
	oldcontext = MemoryContextSwitchTo(qcontext);

	estate = makeNode(EState);

	/*
	 * Initialize all fields of the Executor State structure
	 */
	estate->es_direction = ForwardScanDirection;
	estate->es_snapshot = SnapshotNow;
	estate->es_crosscheck_snapshot = SnapshotAny; /* means no crosscheck */
	estate->es_range_table = NIL;

	estate->es_result_relations = NULL;
	estate->es_num_result_relations = 0;
	estate->es_result_relation_info = NULL;

	estate->es_junkFilter = NULL;
	estate->es_into_relation_descriptor = NULL;

	estate->es_param_list_info = NULL;
	estate->es_param_exec_vals = NULL;

	estate->es_query_cxt = qcontext;

	estate->es_tupleTable = NULL;

	estate->es_processed = 0;
	estate->es_lastoid = InvalidOid;
	estate->es_rowMark = NIL;

	estate->es_instrument = false;
	estate->es_select_into = false;
	estate->es_into_oids = false;

	estate->es_exprcontexts = NIL;

	estate->es_per_tuple_exprcontext = NULL;

	estate->es_topPlan = NULL;
	estate->es_evalPlanQual = NULL;
	estate->es_evTupleNull = NULL;
	estate->es_evTuple = NULL;
	estate->es_useEvalPlan = false;

	/*
	 * Return the executor state structure
	 */
	MemoryContextSwitchTo(oldcontext);

	return estate;
}

/* ----------------
 *		FreeExecutorState
 *
 *		Release an EState along with all remaining working storage.
 *
 * Note: this is not responsible for releasing non-memory resources,
 * such as open relations or buffer pins.  But it will shut down any
 * still-active ExprContexts within the EState.  That is sufficient
 * cleanup for situations where the EState has only been used for expression
 * evaluation, and not to run a complete Plan.
 *
 * This can be called in any memory context ... so long as it's not one
 * of the ones to be freed.
 * ----------------
 */
void
FreeExecutorState(EState *estate)
{
	/*
	 * Shut down and free any remaining ExprContexts.  We do this
	 * explicitly to ensure that any remaining shutdown callbacks get
	 * called (since they might need to release resources that aren't
	 * simply memory within the per-query memory context).
	 */
	while (estate->es_exprcontexts)
	{
		FreeExprContext((ExprContext *) lfirst(estate->es_exprcontexts));
		/* FreeExprContext removed the list link for us */
	}

	/*
	 * Free the per-query memory context, thereby releasing all working
	 * memory, including the EState node itself.
	 */
	MemoryContextDelete(estate->es_query_cxt);
}

/* ----------------
 *		CreateExprContext
 *
 *		Create a context for expression evaluation within an EState.
 *
 * An executor run may require multiple ExprContexts (we usually make one
 * for each Plan node, and a separate one for per-output-tuple processing
 * such as constraint checking).  Each ExprContext has its own "per-tuple"
 * memory context.
 *
 * Note we make no assumption about the caller's memory context.
 * ----------------
 */
ExprContext *
CreateExprContext(EState *estate)
{
	ExprContext *econtext;
	MemoryContext oldcontext;

	/* Create the ExprContext node within the per-query memory context */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	econtext = makeNode(ExprContext);

	/* Initialize fields of ExprContext */
	econtext->ecxt_scantuple = NULL;
	econtext->ecxt_innertuple = NULL;
	econtext->ecxt_outertuple = NULL;

	econtext->ecxt_per_query_memory = estate->es_query_cxt;

	/*
	 * Create working memory for expression evaluation in this context.
	 */
	econtext->ecxt_per_tuple_memory =
		AllocSetContextCreate(estate->es_query_cxt,
							  "ExprContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	econtext->ecxt_param_exec_vals = estate->es_param_exec_vals;
	econtext->ecxt_param_list_info = estate->es_param_list_info;

	econtext->ecxt_aggvalues = NULL;
	econtext->ecxt_aggnulls = NULL;

	econtext->domainValue_datum = (Datum) 0;
	econtext->domainValue_isNull = true;

	econtext->ecxt_estate = estate;

	econtext->ecxt_callbacks = NULL;

	/*
	 * Link the ExprContext into the EState to ensure it is shut down when
	 * the EState is freed.  Because we use lcons(), shutdowns will occur
	 * in reverse order of creation, which may not be essential but can't
	 * hurt.
	 */
	estate->es_exprcontexts = lcons(econtext, estate->es_exprcontexts);

	MemoryContextSwitchTo(oldcontext);

	return econtext;
}

/* ----------------
 *		FreeExprContext
 *
 *		Free an expression context, including calling any remaining
 *		shutdown callbacks.
 *
 * Since we free the temporary context used for expression evaluation,
 * any previously computed pass-by-reference expression result will go away!
 *
 * Note we make no assumption about the caller's memory context.
 * ----------------
 */
void
FreeExprContext(ExprContext *econtext)
{
	EState	   *estate;

	/* Call any registered callbacks */
	ShutdownExprContext(econtext);
	/* And clean up the memory used */
	MemoryContextDelete(econtext->ecxt_per_tuple_memory);
	/* Unlink self from owning EState */
	estate = econtext->ecxt_estate;
	estate->es_exprcontexts = lremove(econtext, estate->es_exprcontexts);
	/* And delete the ExprContext node */
	pfree(econtext);
}

/*
 * ReScanExprContext
 *
 *		Reset an expression context in preparation for a rescan of its
 *		plan node.  This requires calling any registered shutdown callbacks,
 *		since any partially complete set-returning-functions must be canceled.
 *
 * Note we make no assumption about the caller's memory context.
 */
void
ReScanExprContext(ExprContext *econtext)
{
	/* Call any registered callbacks */
	ShutdownExprContext(econtext);
	/* And clean up the memory used */
	MemoryContextReset(econtext->ecxt_per_tuple_memory);
}

/*
 * Build a per-output-tuple ExprContext for an EState.
 *
 * This is normally invoked via GetPerTupleExprContext() macro,
 * not directly.
 */
ExprContext *
MakePerTupleExprContext(EState *estate)
{
	if (estate->es_per_tuple_exprcontext == NULL)
		estate->es_per_tuple_exprcontext = CreateExprContext(estate);

	return estate->es_per_tuple_exprcontext;
}


/* ----------------------------------------------------------------
 *				 miscellaneous node-init support functions
 *
 * Note: all of these are expected to be called with CurrentMemoryContext
 * equal to the per-query memory context.
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ExecAssignExprContext
 *
 *		This initializes the ps_ExprContext field.	It is only necessary
 *		to do this for nodes which use ExecQual or ExecProject
 *		because those routines require an econtext. Other nodes that
 *		don't have to evaluate expressions don't need to do this.
 * ----------------
 */
void
ExecAssignExprContext(EState *estate, PlanState *planstate)
{
	planstate->ps_ExprContext = CreateExprContext(estate);
}

/* ----------------
 *		ExecAssignResultType
 * ----------------
 */
void
ExecAssignResultType(PlanState *planstate,
					 TupleDesc tupDesc, bool shouldFree)
{
	TupleTableSlot *slot = planstate->ps_ResultTupleSlot;

	ExecSetSlotDescriptor(slot, tupDesc, shouldFree);
}

/* ----------------
 *		ExecAssignResultTypeFromOuterPlan
 * ----------------
 */
void
ExecAssignResultTypeFromOuterPlan(PlanState *planstate)
{
	PlanState  *outerPlan;
	TupleDesc	tupDesc;

	outerPlan = outerPlanState(planstate);
	tupDesc = ExecGetResultType(outerPlan);

	ExecAssignResultType(planstate, tupDesc, false);
}

/* ----------------
 *		ExecAssignResultTypeFromTL
 * ----------------
 */
void
ExecAssignResultTypeFromTL(PlanState *planstate)
{
	bool		hasoid;
	TupleDesc	tupDesc;

	if (ExecContextForcesOids(planstate, &hasoid))
	{
		/* context forces OID choice; hasoid is now set correctly */
	}
	else
	{
		/* given free choice, don't leave space for OIDs in result tuples */
		hasoid = false;
	}

	/*
	 * ExecTypeFromTL needs the parse-time representation of the tlist,
	 * not a list of ExprStates.  This is good because some plan nodes
	 * don't bother to set up planstate->targetlist ...
	 */
	tupDesc = ExecTypeFromTL(planstate->plan->targetlist, hasoid);
	ExecAssignResultType(planstate, tupDesc, true);
}

/* ----------------
 *		ExecGetResultType
 * ----------------
 */
TupleDesc
ExecGetResultType(PlanState *planstate)
{
	TupleTableSlot *slot = planstate->ps_ResultTupleSlot;

	return slot->ttc_tupleDescriptor;
}

/* ----------------
 *		ExecBuildProjectionInfo
 *
 * Build a ProjectionInfo node for evaluating the given tlist in the given
 * econtext, and storing the result into the tuple slot.  (Caller must have
 * ensured that tuple slot has a descriptor matching the tlist!)  Note that
 * the given tlist should be a list of ExprState nodes, not Expr nodes.
 * ----------------
 */
ProjectionInfo *
ExecBuildProjectionInfo(List *targetList,
						ExprContext *econtext,
						TupleTableSlot *slot)
{
	ProjectionInfo *projInfo = makeNode(ProjectionInfo);
	int			len;

	len = ExecTargetListLength(targetList);

	projInfo->pi_targetlist = targetList;
	projInfo->pi_exprContext = econtext;
	projInfo->pi_slot = slot;
	if (len > 0)
	{
		projInfo->pi_tupValues = (Datum *) palloc(len * sizeof(Datum));
		projInfo->pi_tupNulls = (char *) palloc(len * sizeof(char));
		projInfo->pi_itemIsDone = (ExprDoneCond *) palloc(len * sizeof(ExprDoneCond));
	}

	return projInfo;
}

/* ----------------
 *		ExecAssignProjectionInfo
 *
 * forms the projection information from the node's targetlist
 * ----------------
 */
void
ExecAssignProjectionInfo(PlanState *planstate)
{
	planstate->ps_ProjInfo =
		ExecBuildProjectionInfo(planstate->targetlist,
								planstate->ps_ExprContext,
								planstate->ps_ResultTupleSlot);
}


/* ----------------
 *		ExecFreeExprContext
 *
 * A plan node's ExprContext should be freed explicitly during ExecEndNode
 * because there may be shutdown callbacks to call.  (Other resources made
 * by the above routines, such as projection info, don't need to be freed
 * explicitly because they're just memory in the per-query memory context.)
 * ----------------
 */
void
ExecFreeExprContext(PlanState *planstate)
{
	ExprContext *econtext;

	/*
	 * get expression context.	if NULL then this node has none so we just
	 * return.
	 */
	econtext = planstate->ps_ExprContext;
	if (econtext == NULL)
		return;

	FreeExprContext(econtext);

	planstate->ps_ExprContext = NULL;
}

/* ----------------------------------------------------------------
 *		the following scan type support functions are for
 *		those nodes which are stubborn and return tuples in
 *		their Scan tuple slot instead of their Result tuple
 *		slot..	luck fur us, these nodes do not do projections
 *		so we don't have to worry about getting the ProjectionInfo
 *		right for them...  -cim 6/3/91
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ExecGetScanType
 * ----------------
 */
TupleDesc
ExecGetScanType(ScanState *scanstate)
{
	TupleTableSlot *slot = scanstate->ss_ScanTupleSlot;

	return slot->ttc_tupleDescriptor;
}

/* ----------------
 *		ExecAssignScanType
 * ----------------
 */
void
ExecAssignScanType(ScanState *scanstate,
				   TupleDesc tupDesc, bool shouldFree)
{
	TupleTableSlot *slot = scanstate->ss_ScanTupleSlot;

	ExecSetSlotDescriptor(slot, tupDesc, shouldFree);
}

/* ----------------
 *		ExecAssignScanTypeFromOuterPlan
 * ----------------
 */
void
ExecAssignScanTypeFromOuterPlan(ScanState *scanstate)
{
	PlanState  *outerPlan;
	TupleDesc	tupDesc;

	outerPlan = outerPlanState(scanstate);
	tupDesc = ExecGetResultType(outerPlan);

	ExecAssignScanType(scanstate, tupDesc, false);
}


/* ----------------------------------------------------------------
 *				  ExecInsertIndexTuples support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecOpenIndices
 *
 *		Find the indices associated with a result relation, open them,
 *		and save information about them in the result ResultRelInfo.
 *
 *		At entry, caller has already opened and locked
 *		resultRelInfo->ri_RelationDesc.
 *
 *		This used to be horribly ugly code, and slow too because it
 *		did a sequential scan of pg_index.	Now we rely on the relcache
 *		to cache a list of the OIDs of the indices associated with any
 *		specific relation, and we use the pg_index syscache to get the
 *		entries we need from pg_index.
 * ----------------------------------------------------------------
 */
void
ExecOpenIndices(ResultRelInfo *resultRelInfo)
{
	Relation	resultRelation = resultRelInfo->ri_RelationDesc;
	List	   *indexoidlist,
			   *indexoidscan;
	int			len,
				i;
	RelationPtr relationDescs;
	IndexInfo **indexInfoArray;

	resultRelInfo->ri_NumIndices = 0;

	/* fast path if no indexes */
	if (!RelationGetForm(resultRelation)->relhasindex)
		return;

	/*
	 * Get cached list of index OIDs
	 */
	indexoidlist = RelationGetIndexList(resultRelation);
	len = length(indexoidlist);
	if (len == 0)
		return;

	/*
	 * allocate space for result arrays
	 */
	relationDescs = (RelationPtr) palloc(len * sizeof(Relation));
	indexInfoArray = (IndexInfo **) palloc(len * sizeof(IndexInfo *));

	resultRelInfo->ri_NumIndices = len;
	resultRelInfo->ri_IndexRelationDescs = relationDescs;
	resultRelInfo->ri_IndexRelationInfo = indexInfoArray;

	/*
	 * For each index, open the index relation and save pg_index info.
	 */
	i = 0;
	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexOid = lfirsto(indexoidscan);
		Relation	indexDesc;
		IndexInfo  *ii;

		/*
		 * Open (and lock, if necessary) the index relation
		 *
		 * If the index AM is not safe for concurrent updates, obtain an
		 * exclusive lock on the index to lock out other updaters as well
		 * as readers (index_beginscan places AccessShareLock). We will
		 * release this lock in ExecCloseIndices.
		 *
		 * If the index AM supports concurrent updates, we obtain no lock
		 * here at all, which is a tad weird, but safe since any critical
		 * operation on the index (like deleting it) will acquire
		 * exclusive lock on the parent table.	Perhaps someday we should
		 * acquire RowExclusiveLock on the index here?
		 *
		 * If there are multiple not-concurrent-safe indexes, all backends
		 * must lock the indexes in the same order or we will get
		 * deadlocks here during concurrent updates.  This is guaranteed
		 * by RelationGetIndexList(), which promises to return the index
		 * list in OID order.
		 */
		indexDesc = index_open(indexOid);

		if (!indexDesc->rd_am->amconcurrent)
			LockRelation(indexDesc, AccessExclusiveLock);

		/* extract index key information from the index's pg_index info */
		ii = BuildIndexInfo(indexDesc);

		relationDescs[i] = indexDesc;
		indexInfoArray[i] = ii;
		i++;
	}

	freeList(indexoidlist);
}

/* ----------------------------------------------------------------
 *		ExecCloseIndices
 *
 *		Close the index relations stored in resultRelInfo
 * ----------------------------------------------------------------
 */
void
ExecCloseIndices(ResultRelInfo *resultRelInfo)
{
	int			i;
	int			numIndices;
	RelationPtr indexDescs;

	numIndices = resultRelInfo->ri_NumIndices;
	indexDescs = resultRelInfo->ri_IndexRelationDescs;

	for (i = 0; i < numIndices; i++)
	{
		if (indexDescs[i] == NULL)
			continue;

		/* Drop lock, if one was acquired by ExecOpenIndices */
		if (!indexDescs[i]->rd_am->amconcurrent)
			UnlockRelation(indexDescs[i], AccessExclusiveLock);

		index_close(indexDescs[i]);
	}

	/*
	 * XXX should free indexInfo array here too?  Currently we assume that
	 * such stuff will be cleaned up automatically in FreeExecutorState.
	 */
}

/* ----------------------------------------------------------------
 *		ExecInsertIndexTuples
 *
 *		This routine takes care of inserting index tuples
 *		into all the relations indexing the result relation
 *		when a heap tuple is inserted into the result relation.
 *		Much of this code should be moved into the genam
 *		stuff as it only exists here because the genam stuff
 *		doesn't provide the functionality needed by the
 *		executor.. -cim 9/27/89
 * ----------------------------------------------------------------
 */
void
ExecInsertIndexTuples(TupleTableSlot *slot,
					  ItemPointer tupleid,
					  EState *estate,
					  bool is_vacuum)
{
	HeapTuple	heapTuple;
	ResultRelInfo *resultRelInfo;
	int			i;
	int			numIndices;
	RelationPtr relationDescs;
	Relation	heapRelation;
	TupleDesc	heapDescriptor;
	IndexInfo **indexInfoArray;
	ExprContext *econtext;
	Datum		datum[INDEX_MAX_KEYS];
	char		nullv[INDEX_MAX_KEYS];

	heapTuple = slot->val;

	/*
	 * Get information from the result relation info structure.
	 */
	resultRelInfo = estate->es_result_relation_info;
	numIndices = resultRelInfo->ri_NumIndices;
	relationDescs = resultRelInfo->ri_IndexRelationDescs;
	indexInfoArray = resultRelInfo->ri_IndexRelationInfo;
	heapRelation = resultRelInfo->ri_RelationDesc;
	heapDescriptor = RelationGetDescr(heapRelation);

	/*
	 * We will use the EState's per-tuple context for evaluating
	 * predicates and index expressions (creating it if it's not already
	 * there).
	 */
	econtext = GetPerTupleExprContext(estate);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/*
	 * for each index, form and insert the index tuple
	 */
	for (i = 0; i < numIndices; i++)
	{
		IndexInfo  *indexInfo;
		InsertIndexResult result;

		if (relationDescs[i] == NULL)
			continue;

		indexInfo = indexInfoArray[i];

		/* Check for partial index */
		if (indexInfo->ii_Predicate != NIL)
		{
			List	   *predicate;

			/*
			 * If predicate state not set up yet, create it (in the
			 * estate's per-query context)
			 */
			predicate = indexInfo->ii_PredicateState;
			if (predicate == NIL)
			{
				predicate = (List *)
					ExecPrepareExpr((Expr *) indexInfo->ii_Predicate,
									estate);
				indexInfo->ii_PredicateState = predicate;
			}

			/* Skip this index-update if the predicate isn't satisfied */
			if (!ExecQual(predicate, econtext, false))
				continue;
		}

		/*
		 * FormIndexDatum fills in its datum and null parameters with
		 * attribute information taken from the given heap tuple. It also
		 * computes any expressions needed.
		 */
		FormIndexDatum(indexInfo,
					   heapTuple,
					   heapDescriptor,
					   estate,
					   datum,
					   nullv);

		/*
		 * The index AM does the rest.	Note we suppress unique-index
		 * checks if we are being called from VACUUM, since VACUUM may
		 * need to move dead tuples that have the same keys as live ones.
		 */
		result = index_insert(relationDescs[i], /* index relation */
							  datum,	/* array of heaptuple Datums */
							  nullv,	/* info on nulls */
							  &(heapTuple->t_self),		/* tid of heap tuple */
							  heapRelation,
				  relationDescs[i]->rd_index->indisunique && !is_vacuum);

		/*
		 * keep track of index inserts for debugging
		 */
		IncrIndexInserted();

		if (result)
			pfree(result);
	}
}

/*
 * UpdateChangedParamSet
 *		Add changed parameters to a plan node's chgParam set
 */
void
UpdateChangedParamSet(PlanState *node, Bitmapset *newchg)
{
	Bitmapset  *parmset;

	/*
	 * The plan node only depends on params listed in its allParam set.
	 * Don't include anything else into its chgParam set.
	 */
	parmset = bms_intersect(node->plan->allParam, newchg);

	/*
	 * Keep node->chgParam == NULL if there's not actually any members;
	 * this allows the simplest possible tests in executor node files.
	 */
	if (!bms_is_empty(parmset))
		node->chgParam = bms_join(node->chgParam, parmset);
	else
		bms_free(parmset);
}

/*
 * Register a shutdown callback in an ExprContext.
 *
 * Shutdown callbacks will be called (in reverse order of registration)
 * when the ExprContext is deleted or rescanned.  This provides a hook
 * for functions called in the context to do any cleanup needed --- it's
 * particularly useful for functions returning sets.  Note that the
 * callback will *not* be called in the event that execution is aborted
 * by an error.
 */
void
RegisterExprContextCallback(ExprContext *econtext,
							ExprContextCallbackFunction function,
							Datum arg)
{
	ExprContext_CB *ecxt_callback;

	/* Save the info in appropriate memory context */
	ecxt_callback = (ExprContext_CB *)
		MemoryContextAlloc(econtext->ecxt_per_query_memory,
						   sizeof(ExprContext_CB));

	ecxt_callback->function = function;
	ecxt_callback->arg = arg;

	/* link to front of list for appropriate execution order */
	ecxt_callback->next = econtext->ecxt_callbacks;
	econtext->ecxt_callbacks = ecxt_callback;
}

/*
 * Deregister a shutdown callback in an ExprContext.
 *
 * Any list entries matching the function and arg will be removed.
 * This can be used if it's no longer necessary to call the callback.
 */
void
UnregisterExprContextCallback(ExprContext *econtext,
							  ExprContextCallbackFunction function,
							  Datum arg)
{
	ExprContext_CB **prev_callback;
	ExprContext_CB *ecxt_callback;

	prev_callback = &econtext->ecxt_callbacks;

	while ((ecxt_callback = *prev_callback) != NULL)
	{
		if (ecxt_callback->function == function && ecxt_callback->arg == arg)
		{
			*prev_callback = ecxt_callback->next;
			pfree(ecxt_callback);
		}
		else
			prev_callback = &ecxt_callback->next;
	}
}

/*
 * Call all the shutdown callbacks registered in an ExprContext.
 *
 * The callback list is emptied (important in case this is only a rescan
 * reset, and not deletion of the ExprContext).
 */
static void
ShutdownExprContext(ExprContext *econtext)
{
	ExprContext_CB *ecxt_callback;
	MemoryContext oldcontext;

	/* Fast path in normal case where there's nothing to do. */
	if (econtext->ecxt_callbacks == NULL)
		return;

	/*
	 * Call the callbacks in econtext's per-tuple context.  This ensures
	 * that any memory they might leak will get cleaned up.
	 */
	oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/*
	 * Call each callback function in reverse registration order.
	 */
	while ((ecxt_callback = econtext->ecxt_callbacks) != NULL)
	{
		econtext->ecxt_callbacks = ecxt_callback->next;
		(*ecxt_callback->function) (ecxt_callback->arg);
		pfree(ecxt_callback);
	}

	MemoryContextSwitchTo(oldcontext);
}
