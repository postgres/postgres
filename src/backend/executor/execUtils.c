/*-------------------------------------------------------------------------
 *
 * execUtils.c
 *	  miscellaneous executor utility routines
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execUtils.c,v 1.90.2.1 2003/01/23 05:10:56 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecAssignExprContext	Common code for plan node init routines.
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
 *
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
 *				 miscellaneous node-init support functions
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ExecAssignExprContext
 *
 *		This initializes the ExprContext field.  It is only necessary
 *		to do this for nodes which use ExecQual or ExecProject
 *		because those routines depend on econtext.	Other nodes that
 *		don't have to evaluate expressions don't need to do this.
 *
 * Note: we assume CurrentMemoryContext is the correct per-query context.
 * This should be true during plan node initialization.
 * ----------------
 */
void
ExecAssignExprContext(EState *estate, CommonState *commonstate)
{
	ExprContext *econtext = makeNode(ExprContext);

	econtext->ecxt_scantuple = NULL;
	econtext->ecxt_innertuple = NULL;
	econtext->ecxt_outertuple = NULL;
	econtext->ecxt_per_query_memory = CurrentMemoryContext;

	/*
	 * Create working memory for expression evaluation in this context.
	 */
	econtext->ecxt_per_tuple_memory =
		AllocSetContextCreate(CurrentMemoryContext,
							  "PlanExprContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);
	econtext->ecxt_param_exec_vals = estate->es_param_exec_vals;
	econtext->ecxt_param_list_info = estate->es_param_list_info;
	econtext->ecxt_aggvalues = NULL;
	econtext->ecxt_aggnulls = NULL;
	econtext->ecxt_callbacks = NULL;

	commonstate->cs_ExprContext = econtext;
}

/* ----------------
 *		MakeExprContext
 *
 *		Build an expression context for use outside normal plan-node cases.
 *		A fake scan-tuple slot can be supplied (pass NULL if not needed).
 *		A memory context sufficiently long-lived to use as fcache context
 *		must be supplied as well.
 * ----------------
 */
ExprContext *
MakeExprContext(TupleTableSlot *slot,
				MemoryContext queryContext)
{
	ExprContext *econtext = makeNode(ExprContext);

	econtext->ecxt_scantuple = slot;
	econtext->ecxt_innertuple = NULL;
	econtext->ecxt_outertuple = NULL;
	econtext->ecxt_per_query_memory = queryContext;

	/*
	 * We make the temporary context a child of current working context,
	 * not of the specified queryContext.  This seems reasonable but I'm
	 * not totally sure about it...
	 *
	 * Expression contexts made via this routine typically don't live long
	 * enough to get reset, so specify a minsize of 0.	That avoids
	 * alloc'ing any memory in the common case where expr eval doesn't use
	 * any.
	 */
	econtext->ecxt_per_tuple_memory =
		AllocSetContextCreate(CurrentMemoryContext,
							  "TempExprContext",
							  0,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);
	econtext->ecxt_param_exec_vals = NULL;
	econtext->ecxt_param_list_info = NULL;
	econtext->ecxt_aggvalues = NULL;
	econtext->ecxt_aggnulls = NULL;
	econtext->ecxt_callbacks = NULL;

	return econtext;
}

/*
 * Free an ExprContext made by MakeExprContext, including the temporary
 * context used for expression evaluation.	Note this will cause any
 * pass-by-reference expression result to go away!
 */
void
FreeExprContext(ExprContext *econtext)
{
	/* Call any registered callbacks */
	ShutdownExprContext(econtext);
	/* And clean up the memory used */
	MemoryContextDelete(econtext->ecxt_per_tuple_memory);
	pfree(econtext);
}

/*
 * Build a per-output-tuple ExprContext for an EState.
 *
 * This is normally invoked via GetPerTupleExprContext() macro.
 */
ExprContext *
MakePerTupleExprContext(EState *estate)
{
	if (estate->es_per_tuple_exprcontext == NULL)
	{
		MemoryContext oldContext;

		oldContext = MemoryContextSwitchTo(estate->es_query_cxt);
		estate->es_per_tuple_exprcontext =
			MakeExprContext(NULL, estate->es_query_cxt);
		MemoryContextSwitchTo(oldContext);
	}
	return estate->es_per_tuple_exprcontext;
}

/* ----------------------------------------------------------------
 *		Result slot tuple type and ProjectionInfo support
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ExecAssignResultType
 * ----------------
 */
void
ExecAssignResultType(CommonState *commonstate,
					 TupleDesc tupDesc, bool shouldFree)
{
	TupleTableSlot *slot = commonstate->cs_ResultTupleSlot;

	ExecSetSlotDescriptor(slot, tupDesc, shouldFree);
}

/* ----------------
 *		ExecAssignResultTypeFromOuterPlan
 * ----------------
 */
void
ExecAssignResultTypeFromOuterPlan(Plan *node, CommonState *commonstate)
{
	Plan	   *outerPlan;
	TupleDesc	tupDesc;

	outerPlan = outerPlan(node);
	tupDesc = ExecGetTupType(outerPlan);

	ExecAssignResultType(commonstate, tupDesc, false);
}

/* ----------------
 *		ExecAssignResultTypeFromTL
 * ----------------
 */
void
ExecAssignResultTypeFromTL(Plan *node, CommonState *commonstate)
{
	ResultRelInfo *ri;
	bool		hasoid = false;
	TupleDesc	tupDesc;

	/*
	 * This is pretty grotty: we need to ensure that result tuples have
	 * space for an OID iff they are going to be stored into a relation
	 * that has OIDs.  We assume that estate->es_result_relation_info is
	 * already set up to describe the target relation.	One reason this is
	 * ugly is that all plan nodes in the plan tree will emit tuples with
	 * space for an OID, though we really only need the topmost plan to do
	 * so.
	 *
	 * It would be better to have InitPlan adjust the topmost plan node's
	 * output descriptor after plan tree initialization.  However, that
	 * doesn't quite work because in an UPDATE that spans an inheritance
	 * tree, some of the target relations may have OIDs and some not. We
	 * have to make the decision on a per-relation basis as we initialize
	 * each of the child plans of the topmost Append plan.	So, this is
	 * ugly but it works, for now ...
	 *
	 * SELECT INTO is also pretty grotty, because we don't yet have the
	 * INTO relation's descriptor at this point; we have to look aside
	 * at a flag set by InitPlan().
	 */
	if (node->state->es_force_oids)
		hasoid = true;
	else
	{
		ri = node->state->es_result_relation_info;
		if (ri != NULL)
		{
			Relation	rel = ri->ri_RelationDesc;

			if (rel != NULL)
				hasoid = rel->rd_rel->relhasoids;
		}
	}

	tupDesc = ExecTypeFromTL(node->targetlist, hasoid);
	ExecAssignResultType(commonstate, tupDesc, true);
}

/* ----------------
 *		ExecGetResultType
 * ----------------
 */
TupleDesc
ExecGetResultType(CommonState *commonstate)
{
	TupleTableSlot *slot = commonstate->cs_ResultTupleSlot;

	return slot->ttc_tupleDescriptor;
}

/* ----------------
 *		ExecAssignProjectionInfo
		  forms the projection information from the node's targetlist
 * ----------------
 */
void
ExecAssignProjectionInfo(Plan *node, CommonState *commonstate)
{
	ProjectionInfo *projInfo;
	List	   *targetList;
	int			len;

	targetList = node->targetlist;
	len = ExecTargetListLength(targetList);

	projInfo = makeNode(ProjectionInfo);
	projInfo->pi_targetlist = targetList;
	projInfo->pi_len = len;
	projInfo->pi_tupValue = (len <= 0) ? NULL : (Datum *) palloc(sizeof(Datum) * len);
	projInfo->pi_exprContext = commonstate->cs_ExprContext;
	projInfo->pi_slot = commonstate->cs_ResultTupleSlot;

	commonstate->cs_ProjInfo = projInfo;
}


/* ----------------
 *		ExecFreeProjectionInfo
 * ----------------
 */
void
ExecFreeProjectionInfo(CommonState *commonstate)
{
	ProjectionInfo *projInfo;

	/*
	 * get projection info.  if NULL then this node has none so we just
	 * return.
	 */
	projInfo = commonstate->cs_ProjInfo;
	if (projInfo == NULL)
		return;

	/*
	 * clean up memory used.
	 */
	if (projInfo->pi_tupValue != NULL)
		pfree(projInfo->pi_tupValue);

	pfree(projInfo);
	commonstate->cs_ProjInfo = NULL;
}

/* ----------------
 *		ExecFreeExprContext
 * ----------------
 */
void
ExecFreeExprContext(CommonState *commonstate)
{
	ExprContext *econtext;

	/*
	 * get expression context.	if NULL then this node has none so we just
	 * return.
	 */
	econtext = commonstate->cs_ExprContext;
	if (econtext == NULL)
		return;

	/*
	 * clean up any registered callbacks
	 */
	ShutdownExprContext(econtext);

	/*
	 * clean up memory used.
	 */
	MemoryContextDelete(econtext->ecxt_per_tuple_memory);
	pfree(econtext);
	commonstate->cs_ExprContext = NULL;
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
ExecGetScanType(CommonScanState *csstate)
{
	TupleTableSlot *slot = csstate->css_ScanTupleSlot;

	return slot->ttc_tupleDescriptor;
}

/* ----------------
 *		ExecAssignScanType
 * ----------------
 */
void
ExecAssignScanType(CommonScanState *csstate,
				   TupleDesc tupDesc, bool shouldFree)
{
	TupleTableSlot *slot = csstate->css_ScanTupleSlot;

	ExecSetSlotDescriptor(slot, tupDesc, shouldFree);
}

/* ----------------
 *		ExecAssignScanTypeFromOuterPlan
 * ----------------
 */
void
ExecAssignScanTypeFromOuterPlan(Plan *node, CommonScanState *csstate)
{
	Plan	   *outerPlan;
	TupleDesc	tupDesc;

	outerPlan = outerPlan(node);
	tupDesc = ExecGetTupType(outerPlan);

	ExecAssignScanType(csstate, tupDesc, false);
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

	/* checks for disabled indexes */
	if (!RelationGetForm(resultRelation)->relhasindex)
		return;
	if (IsIgnoringSystemIndexes() &&
		IsSystemRelation(resultRelation))
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
		Oid			indexOid = lfirsti(indexoidscan);
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

		/*
		 * extract index key information from the index's pg_index tuple
		 */
		ii = BuildIndexInfo(indexDesc->rd_index);

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
	 * XXX should free indexInfo array here too.
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
	 * predicates and functional-index functions (creating it if it's not
	 * already there).
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
		List	   *predicate;
		InsertIndexResult result;

		if (relationDescs[i] == NULL)
			continue;

		indexInfo = indexInfoArray[i];
		predicate = indexInfo->ii_Predicate;
		if (predicate != NIL)
		{
			/* Skip this index-update if the predicate isn't satisfied */
			if (!ExecQual(predicate, econtext, false))
				continue;
		}

		/*
		 * FormIndexDatum fills in its datum and null parameters with
		 * attribute information taken from the given heap tuple.
		 */
		FormIndexDatum(indexInfo,
					   heapTuple,
					   heapDescriptor,
					   econtext->ecxt_per_tuple_memory,
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

void
SetChangedParamList(Plan *node, List *newchg)
{
	List	   *nl;

	foreach(nl, newchg)
	{
		int			paramId = lfirsti(nl);

		/* if this node doesn't depend on a param ... */
		if (!intMember(paramId, node->extParam) &&
			!intMember(paramId, node->locParam))
			continue;
		/* if this param is already in list of changed ones ... */
		if (intMember(paramId, node->chgParam))
			continue;
		/* else - add this param to the list */
		node->chgParam = lappendi(node->chgParam, paramId);
	}
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

	/*
	 * Call each callback function in reverse registration order.
	 */
	while ((ecxt_callback = econtext->ecxt_callbacks) != NULL)
	{
		econtext->ecxt_callbacks = ecxt_callback->next;
		(*ecxt_callback->function) (ecxt_callback->arg);
		pfree(ecxt_callback);
	}
}
