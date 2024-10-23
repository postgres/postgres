/*-------------------------------------------------------------------------
 *
 * execUtils.c
 *	  miscellaneous executor utility routines
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execUtils.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		CreateExecutorState		Create/delete executor working state
 *		FreeExecutorState
 *		CreateExprContext
 *		CreateStandaloneExprContext
 *		FreeExprContext
 *		ReScanExprContext
 *
 *		ExecAssignExprContext	Common code for plan node init routines.
 *		etc
 *
 *		ExecOpenScanRelation	Common code for scan node init routines.
 *
 *		ExecInitRangeTable		Set up executor's range-table-related data.
 *
 *		ExecGetRangeTableRelation		Fetch Relation for a rangetable entry.
 *
 *		executor_errposition	Report syntactic position of an error.
 *
 *		RegisterExprContextCallback    Register function shutdown callback
 *		UnregisterExprContextCallback  Deregister function shutdown callback
 *
 *		GetAttributeByName		Runtime extraction of columns from tuples.
 *		GetAttributeByNum
 *
 *	 NOTES
 *		This file has traditionally been the place to stick misc.
 *		executor support stuff that doesn't really go anyplace else.
 */

#include "postgres.h"

#include "access/parallel.h"
#include "access/table.h"
#include "access/tableam.h"
#include "executor/executor.h"
#include "executor/nodeModifyTable.h"
#include "jit/jit.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/parse_relation.h"
#include "partitioning/partdesc.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/typcache.h"


static bool tlist_matches_tupdesc(PlanState *ps, List *tlist, int varno, TupleDesc tupdesc);
static void ShutdownExprContext(ExprContext *econtext, bool isCommit);
static RTEPermissionInfo *GetResultRTEPermissionInfo(ResultRelInfo *relinfo, EState *estate);


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
									 ALLOCSET_DEFAULT_SIZES);

	/*
	 * Make the EState node within the per-query context.  This way, we don't
	 * need a separate pfree() operation for it at shutdown.
	 */
	oldcontext = MemoryContextSwitchTo(qcontext);

	estate = makeNode(EState);

	/*
	 * Initialize all fields of the Executor State structure
	 */
	estate->es_direction = ForwardScanDirection;
	estate->es_snapshot = InvalidSnapshot;	/* caller must initialize this */
	estate->es_crosscheck_snapshot = InvalidSnapshot;	/* no crosscheck */
	estate->es_range_table = NIL;
	estate->es_range_table_size = 0;
	estate->es_relations = NULL;
	estate->es_rowmarks = NULL;
	estate->es_rteperminfos = NIL;
	estate->es_plannedstmt = NULL;

	estate->es_junkFilter = NULL;

	estate->es_output_cid = (CommandId) 0;

	estate->es_result_relations = NULL;
	estate->es_opened_result_relations = NIL;
	estate->es_tuple_routing_result_relations = NIL;
	estate->es_trig_target_relations = NIL;

	estate->es_insert_pending_result_relations = NIL;
	estate->es_insert_pending_modifytables = NIL;

	estate->es_param_list_info = NULL;
	estate->es_param_exec_vals = NULL;

	estate->es_queryEnv = NULL;

	estate->es_query_cxt = qcontext;

	estate->es_tupleTable = NIL;

	estate->es_processed = 0;
	estate->es_total_processed = 0;

	estate->es_top_eflags = 0;
	estate->es_instrument = 0;
	estate->es_finished = false;

	estate->es_exprcontexts = NIL;

	estate->es_subplanstates = NIL;

	estate->es_auxmodifytables = NIL;

	estate->es_per_tuple_exprcontext = NULL;

	estate->es_sourceText = NULL;

	estate->es_use_parallel_mode = false;
	estate->es_parallel_workers_to_launch = 0;
	estate->es_parallel_workers_launched = 0;

	estate->es_jit_flags = 0;
	estate->es_jit = NULL;

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
 * Note: this is not responsible for releasing non-memory resources, such as
 * open relations or buffer pins.  But it will shut down any still-active
 * ExprContexts within the EState and deallocate associated JITed expressions.
 * That is sufficient cleanup for situations where the EState has only been
 * used for expression evaluation, and not to run a complete Plan.
 *
 * This can be called in any memory context ... so long as it's not one
 * of the ones to be freed.
 * ----------------
 */
void
FreeExecutorState(EState *estate)
{
	/*
	 * Shut down and free any remaining ExprContexts.  We do this explicitly
	 * to ensure that any remaining shutdown callbacks get called (since they
	 * might need to release resources that aren't simply memory within the
	 * per-query memory context).
	 */
	while (estate->es_exprcontexts)
	{
		/*
		 * XXX: seems there ought to be a faster way to implement this than
		 * repeated list_delete(), no?
		 */
		FreeExprContext((ExprContext *) linitial(estate->es_exprcontexts),
						true);
		/* FreeExprContext removed the list link for us */
	}

	/* release JIT context, if allocated */
	if (estate->es_jit)
	{
		jit_release_context(estate->es_jit);
		estate->es_jit = NULL;
	}

	/* release partition directory, if allocated */
	if (estate->es_partition_directory)
	{
		DestroyPartitionDirectory(estate->es_partition_directory);
		estate->es_partition_directory = NULL;
	}

	/*
	 * Free the per-query memory context, thereby releasing all working
	 * memory, including the EState node itself.
	 */
	MemoryContextDelete(estate->es_query_cxt);
}

/*
 * Internal implementation for CreateExprContext() and CreateWorkExprContext()
 * that allows control over the AllocSet parameters.
 */
static ExprContext *
CreateExprContextInternal(EState *estate, Size minContextSize,
						  Size initBlockSize, Size maxBlockSize)
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
							  minContextSize,
							  initBlockSize,
							  maxBlockSize);

	econtext->ecxt_param_exec_vals = estate->es_param_exec_vals;
	econtext->ecxt_param_list_info = estate->es_param_list_info;

	econtext->ecxt_aggvalues = NULL;
	econtext->ecxt_aggnulls = NULL;

	econtext->caseValue_datum = (Datum) 0;
	econtext->caseValue_isNull = true;

	econtext->domainValue_datum = (Datum) 0;
	econtext->domainValue_isNull = true;

	econtext->ecxt_estate = estate;

	econtext->ecxt_callbacks = NULL;

	/*
	 * Link the ExprContext into the EState to ensure it is shut down when the
	 * EState is freed.  Because we use lcons(), shutdowns will occur in
	 * reverse order of creation, which may not be essential but can't hurt.
	 */
	estate->es_exprcontexts = lcons(econtext, estate->es_exprcontexts);

	MemoryContextSwitchTo(oldcontext);

	return econtext;
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
	return CreateExprContextInternal(estate, ALLOCSET_DEFAULT_SIZES);
}


/* ----------------
 *		CreateWorkExprContext
 *
 * Like CreateExprContext, but specifies the AllocSet sizes to be reasonable
 * in proportion to work_mem. If the maximum block allocation size is too
 * large, it's easy to skip right past work_mem with a single allocation.
 * ----------------
 */
ExprContext *
CreateWorkExprContext(EState *estate)
{
	Size		minContextSize = ALLOCSET_DEFAULT_MINSIZE;
	Size		initBlockSize = ALLOCSET_DEFAULT_INITSIZE;
	Size		maxBlockSize = ALLOCSET_DEFAULT_MAXSIZE;

	/* choose the maxBlockSize to be no larger than 1/16 of work_mem */
	while (16 * maxBlockSize > work_mem * 1024L)
		maxBlockSize >>= 1;

	if (maxBlockSize < ALLOCSET_DEFAULT_INITSIZE)
		maxBlockSize = ALLOCSET_DEFAULT_INITSIZE;

	return CreateExprContextInternal(estate, minContextSize,
									 initBlockSize, maxBlockSize);
}

/* ----------------
 *		CreateStandaloneExprContext
 *
 *		Create a context for standalone expression evaluation.
 *
 * An ExprContext made this way can be used for evaluation of expressions
 * that contain no Params, subplans, or Var references (it might work to
 * put tuple references into the scantuple field, but it seems unwise).
 *
 * The ExprContext struct is allocated in the caller's current memory
 * context, which also becomes its "per query" context.
 *
 * It is caller's responsibility to free the ExprContext when done,
 * or at least ensure that any shutdown callbacks have been called
 * (ReScanExprContext() is suitable).  Otherwise, non-memory resources
 * might be leaked.
 * ----------------
 */
ExprContext *
CreateStandaloneExprContext(void)
{
	ExprContext *econtext;

	/* Create the ExprContext node within the caller's memory context */
	econtext = makeNode(ExprContext);

	/* Initialize fields of ExprContext */
	econtext->ecxt_scantuple = NULL;
	econtext->ecxt_innertuple = NULL;
	econtext->ecxt_outertuple = NULL;

	econtext->ecxt_per_query_memory = CurrentMemoryContext;

	/*
	 * Create working memory for expression evaluation in this context.
	 */
	econtext->ecxt_per_tuple_memory =
		AllocSetContextCreate(CurrentMemoryContext,
							  "ExprContext",
							  ALLOCSET_DEFAULT_SIZES);

	econtext->ecxt_param_exec_vals = NULL;
	econtext->ecxt_param_list_info = NULL;

	econtext->ecxt_aggvalues = NULL;
	econtext->ecxt_aggnulls = NULL;

	econtext->caseValue_datum = (Datum) 0;
	econtext->caseValue_isNull = true;

	econtext->domainValue_datum = (Datum) 0;
	econtext->domainValue_isNull = true;

	econtext->ecxt_estate = NULL;

	econtext->ecxt_callbacks = NULL;

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
 * If isCommit is false, we are being called in error cleanup, and should
 * not call callbacks but only release memory.  (It might be better to call
 * the callbacks and pass the isCommit flag to them, but that would require
 * more invasive code changes than currently seems justified.)
 *
 * Note we make no assumption about the caller's memory context.
 * ----------------
 */
void
FreeExprContext(ExprContext *econtext, bool isCommit)
{
	EState	   *estate;

	/* Call any registered callbacks */
	ShutdownExprContext(econtext, isCommit);
	/* And clean up the memory used */
	MemoryContextDelete(econtext->ecxt_per_tuple_memory);
	/* Unlink self from owning EState, if any */
	estate = econtext->ecxt_estate;
	if (estate)
		estate->es_exprcontexts = list_delete_ptr(estate->es_exprcontexts,
												  econtext);
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
	ShutdownExprContext(econtext, true);
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
 *		This initializes the ps_ExprContext field.  It is only necessary
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
 *		ExecGetResultType
 * ----------------
 */
TupleDesc
ExecGetResultType(PlanState *planstate)
{
	return planstate->ps_ResultTupleDesc;
}

/*
 * ExecGetResultSlotOps - information about node's type of result slot
 */
const TupleTableSlotOps *
ExecGetResultSlotOps(PlanState *planstate, bool *isfixed)
{
	if (planstate->resultopsset && planstate->resultops)
	{
		if (isfixed)
			*isfixed = planstate->resultopsfixed;
		return planstate->resultops;
	}

	if (isfixed)
	{
		if (planstate->resultopsset)
			*isfixed = planstate->resultopsfixed;
		else if (planstate->ps_ResultTupleSlot)
			*isfixed = TTS_FIXED(planstate->ps_ResultTupleSlot);
		else
			*isfixed = false;
	}

	if (!planstate->ps_ResultTupleSlot)
		return &TTSOpsVirtual;

	return planstate->ps_ResultTupleSlot->tts_ops;
}


/* ----------------
 *		ExecAssignProjectionInfo
 *
 * forms the projection information from the node's targetlist
 *
 * Notes for inputDesc are same as for ExecBuildProjectionInfo: supply it
 * for a relation-scan node, can pass NULL for upper-level nodes
 * ----------------
 */
void
ExecAssignProjectionInfo(PlanState *planstate,
						 TupleDesc inputDesc)
{
	planstate->ps_ProjInfo =
		ExecBuildProjectionInfo(planstate->plan->targetlist,
								planstate->ps_ExprContext,
								planstate->ps_ResultTupleSlot,
								planstate,
								inputDesc);
}


/* ----------------
 *		ExecConditionalAssignProjectionInfo
 *
 * as ExecAssignProjectionInfo, but store NULL rather than building projection
 * info if no projection is required
 * ----------------
 */
void
ExecConditionalAssignProjectionInfo(PlanState *planstate, TupleDesc inputDesc,
									int varno)
{
	if (tlist_matches_tupdesc(planstate,
							  planstate->plan->targetlist,
							  varno,
							  inputDesc))
	{
		planstate->ps_ProjInfo = NULL;
		planstate->resultopsset = planstate->scanopsset;
		planstate->resultopsfixed = planstate->scanopsfixed;
		planstate->resultops = planstate->scanops;
	}
	else
	{
		if (!planstate->ps_ResultTupleSlot)
		{
			ExecInitResultSlot(planstate, &TTSOpsVirtual);
			planstate->resultops = &TTSOpsVirtual;
			planstate->resultopsfixed = true;
			planstate->resultopsset = true;
		}
		ExecAssignProjectionInfo(planstate, inputDesc);
	}
}

static bool
tlist_matches_tupdesc(PlanState *ps, List *tlist, int varno, TupleDesc tupdesc)
{
	int			numattrs = tupdesc->natts;
	int			attrno;
	ListCell   *tlist_item = list_head(tlist);

	/* Check the tlist attributes */
	for (attrno = 1; attrno <= numattrs; attrno++)
	{
		Form_pg_attribute att_tup = TupleDescAttr(tupdesc, attrno - 1);
		Var		   *var;

		if (tlist_item == NULL)
			return false;		/* tlist too short */
		var = (Var *) ((TargetEntry *) lfirst(tlist_item))->expr;
		if (!var || !IsA(var, Var))
			return false;		/* tlist item not a Var */
		/* if these Asserts fail, planner messed up */
		Assert(var->varno == varno);
		Assert(var->varlevelsup == 0);
		if (var->varattno != attrno)
			return false;		/* out of order */
		if (att_tup->attisdropped)
			return false;		/* table contains dropped columns */
		if (att_tup->atthasmissing)
			return false;		/* table contains cols with missing values */

		/*
		 * Note: usually the Var's type should match the tupdesc exactly, but
		 * in situations involving unions of columns that have different
		 * typmods, the Var may have come from above the union and hence have
		 * typmod -1.  This is a legitimate situation since the Var still
		 * describes the column, just not as exactly as the tupdesc does. We
		 * could change the planner to prevent it, but it'd then insert
		 * projection steps just to convert from specific typmod to typmod -1,
		 * which is pretty silly.
		 */
		if (var->vartype != att_tup->atttypid ||
			(var->vartypmod != att_tup->atttypmod &&
			 var->vartypmod != -1))
			return false;		/* type mismatch */

		tlist_item = lnext(tlist, tlist_item);
	}

	if (tlist_item)
		return false;			/* tlist too long */

	return true;
}


/* ----------------------------------------------------------------
 *				  Scan node support
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ExecAssignScanType
 * ----------------
 */
void
ExecAssignScanType(ScanState *scanstate, TupleDesc tupDesc)
{
	TupleTableSlot *slot = scanstate->ss_ScanTupleSlot;

	ExecSetSlotDescriptor(slot, tupDesc);
}

/* ----------------
 *		ExecCreateScanSlotFromOuterPlan
 * ----------------
 */
void
ExecCreateScanSlotFromOuterPlan(EState *estate,
								ScanState *scanstate,
								const TupleTableSlotOps *tts_ops)
{
	PlanState  *outerPlan;
	TupleDesc	tupDesc;

	outerPlan = outerPlanState(scanstate);
	tupDesc = ExecGetResultType(outerPlan);

	ExecInitScanTupleSlot(estate, scanstate, tupDesc, tts_ops);
}

/* ----------------------------------------------------------------
 *		ExecRelationIsTargetRelation
 *
 *		Detect whether a relation (identified by rangetable index)
 *		is one of the target relations of the query.
 *
 * Note: This is currently no longer used in core.  We keep it around
 * because FDWs may wish to use it to determine if their foreign table
 * is a target relation.
 * ----------------------------------------------------------------
 */
bool
ExecRelationIsTargetRelation(EState *estate, Index scanrelid)
{
	return list_member_int(estate->es_plannedstmt->resultRelations, scanrelid);
}

/* ----------------------------------------------------------------
 *		ExecOpenScanRelation
 *
 *		Open the heap relation to be scanned by a base-level scan plan node.
 *		This should be called during the node's ExecInit routine.
 * ----------------------------------------------------------------
 */
Relation
ExecOpenScanRelation(EState *estate, Index scanrelid, int eflags)
{
	Relation	rel;

	/* Open the relation. */
	rel = ExecGetRangeTableRelation(estate, scanrelid);

	/*
	 * Complain if we're attempting a scan of an unscannable relation, except
	 * when the query won't actually be run.  This is a slightly klugy place
	 * to do this, perhaps, but there is no better place.
	 */
	if ((eflags & (EXEC_FLAG_EXPLAIN_ONLY | EXEC_FLAG_WITH_NO_DATA)) == 0 &&
		!RelationIsScannable(rel))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("materialized view \"%s\" has not been populated",
						RelationGetRelationName(rel)),
				 errhint("Use the REFRESH MATERIALIZED VIEW command.")));

	return rel;
}

/*
 * ExecInitRangeTable
 *		Set up executor's range-table-related data
 *
 * In addition to the range table proper, initialize arrays that are
 * indexed by rangetable index.
 */
void
ExecInitRangeTable(EState *estate, List *rangeTable, List *permInfos)
{
	/* Remember the range table List as-is */
	estate->es_range_table = rangeTable;

	/* ... and the RTEPermissionInfo List too */
	estate->es_rteperminfos = permInfos;

	/* Set size of associated arrays */
	estate->es_range_table_size = list_length(rangeTable);

	/*
	 * Allocate an array to store an open Relation corresponding to each
	 * rangetable entry, and initialize entries to NULL.  Relations are opened
	 * and stored here as needed.
	 */
	estate->es_relations = (Relation *)
		palloc0(estate->es_range_table_size * sizeof(Relation));

	/*
	 * es_result_relations and es_rowmarks are also parallel to
	 * es_range_table, but are allocated only if needed.
	 */
	estate->es_result_relations = NULL;
	estate->es_rowmarks = NULL;
}

/*
 * ExecGetRangeTableRelation
 *		Open the Relation for a range table entry, if not already done
 *
 * The Relations will be closed in ExecEndPlan().
 */
Relation
ExecGetRangeTableRelation(EState *estate, Index rti)
{
	Relation	rel;

	Assert(rti > 0 && rti <= estate->es_range_table_size);

	rel = estate->es_relations[rti - 1];
	if (rel == NULL)
	{
		/* First time through, so open the relation */
		RangeTblEntry *rte = exec_rt_fetch(rti, estate);

		Assert(rte->rtekind == RTE_RELATION);

		if (!IsParallelWorker())
		{
			/*
			 * In a normal query, we should already have the appropriate lock,
			 * but verify that through an Assert.  Since there's already an
			 * Assert inside table_open that insists on holding some lock, it
			 * seems sufficient to check this only when rellockmode is higher
			 * than the minimum.
			 */
			rel = table_open(rte->relid, NoLock);
			Assert(rte->rellockmode == AccessShareLock ||
				   CheckRelationLockedByMe(rel, rte->rellockmode, false));
		}
		else
		{
			/*
			 * If we are a parallel worker, we need to obtain our own local
			 * lock on the relation.  This ensures sane behavior in case the
			 * parent process exits before we do.
			 */
			rel = table_open(rte->relid, rte->rellockmode);
		}

		estate->es_relations[rti - 1] = rel;
	}

	return rel;
}

/*
 * ExecInitResultRelation
 *		Open relation given by the passed-in RT index and fill its
 *		ResultRelInfo node
 *
 * Here, we also save the ResultRelInfo in estate->es_result_relations array
 * such that it can be accessed later using the RT index.
 */
void
ExecInitResultRelation(EState *estate, ResultRelInfo *resultRelInfo,
					   Index rti)
{
	Relation	resultRelationDesc;

	resultRelationDesc = ExecGetRangeTableRelation(estate, rti);
	InitResultRelInfo(resultRelInfo,
					  resultRelationDesc,
					  rti,
					  NULL,
					  estate->es_instrument);

	if (estate->es_result_relations == NULL)
		estate->es_result_relations = (ResultRelInfo **)
			palloc0(estate->es_range_table_size * sizeof(ResultRelInfo *));
	estate->es_result_relations[rti - 1] = resultRelInfo;

	/*
	 * Saving in the list allows to avoid needlessly traversing the whole
	 * array when only a few of its entries are possibly non-NULL.
	 */
	estate->es_opened_result_relations =
		lappend(estate->es_opened_result_relations, resultRelInfo);
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
	 * The plan node only depends on params listed in its allParam set. Don't
	 * include anything else into its chgParam set.
	 */
	parmset = bms_intersect(node->plan->allParam, newchg);
	node->chgParam = bms_join(node->chgParam, parmset);
}

/*
 * executor_errposition
 *		Report an execution-time cursor position, if possible.
 *
 * This is expected to be used within an ereport() call.  The return value
 * is a dummy (always 0, in fact).
 *
 * The locations stored in parsetrees are byte offsets into the source string.
 * We have to convert them to 1-based character indexes for reporting to
 * clients.  (We do things this way to avoid unnecessary overhead in the
 * normal non-error case: computing character indexes would be much more
 * expensive than storing token offsets.)
 */
int
executor_errposition(EState *estate, int location)
{
	int			pos;

	/* No-op if location was not provided */
	if (location < 0)
		return 0;
	/* Can't do anything if source text is not available */
	if (estate == NULL || estate->es_sourceText == NULL)
		return 0;
	/* Convert offset to character number */
	pos = pg_mbstrlen_with_len(estate->es_sourceText, location) + 1;
	/* And pass it to the ereport mechanism */
	return errposition(pos);
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
 *
 * If isCommit is false, just clean the callback list but don't call 'em.
 * (See comment for FreeExprContext.)
 */
static void
ShutdownExprContext(ExprContext *econtext, bool isCommit)
{
	ExprContext_CB *ecxt_callback;
	MemoryContext oldcontext;

	/* Fast path in normal case where there's nothing to do. */
	if (econtext->ecxt_callbacks == NULL)
		return;

	/*
	 * Call the callbacks in econtext's per-tuple context.  This ensures that
	 * any memory they might leak will get cleaned up.
	 */
	oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/*
	 * Call each callback function in reverse registration order.
	 */
	while ((ecxt_callback = econtext->ecxt_callbacks) != NULL)
	{
		econtext->ecxt_callbacks = ecxt_callback->next;
		if (isCommit)
			ecxt_callback->function(ecxt_callback->arg);
		pfree(ecxt_callback);
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 *		GetAttributeByName
 *		GetAttributeByNum
 *
 *		These functions return the value of the requested attribute
 *		out of the given tuple Datum.
 *		C functions which take a tuple as an argument are expected
 *		to use these.  Ex: overpaid(EMP) might call GetAttributeByNum().
 *		Note: these are actually rather slow because they do a typcache
 *		lookup on each call.
 */
Datum
GetAttributeByName(HeapTupleHeader tuple, const char *attname, bool *isNull)
{
	AttrNumber	attrno;
	Datum		result;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupDesc;
	HeapTupleData tmptup;
	int			i;

	if (attname == NULL)
		elog(ERROR, "invalid attribute name");

	if (isNull == NULL)
		elog(ERROR, "a NULL isNull pointer was passed");

	if (tuple == NULL)
	{
		/* Kinda bogus but compatible with old behavior... */
		*isNull = true;
		return (Datum) 0;
	}

	tupType = HeapTupleHeaderGetTypeId(tuple);
	tupTypmod = HeapTupleHeaderGetTypMod(tuple);
	tupDesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

	attrno = InvalidAttrNumber;
	for (i = 0; i < tupDesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupDesc, i);

		if (namestrcmp(&(att->attname), attname) == 0)
		{
			attrno = att->attnum;
			break;
		}
	}

	if (attrno == InvalidAttrNumber)
		elog(ERROR, "attribute \"%s\" does not exist", attname);

	/*
	 * heap_getattr needs a HeapTuple not a bare HeapTupleHeader.  We set all
	 * the fields in the struct just in case user tries to inspect system
	 * columns.
	 */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = tuple;

	result = heap_getattr(&tmptup,
						  attrno,
						  tupDesc,
						  isNull);

	ReleaseTupleDesc(tupDesc);

	return result;
}

Datum
GetAttributeByNum(HeapTupleHeader tuple,
				  AttrNumber attrno,
				  bool *isNull)
{
	Datum		result;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupDesc;
	HeapTupleData tmptup;

	if (!AttributeNumberIsValid(attrno))
		elog(ERROR, "invalid attribute number %d", attrno);

	if (isNull == NULL)
		elog(ERROR, "a NULL isNull pointer was passed");

	if (tuple == NULL)
	{
		/* Kinda bogus but compatible with old behavior... */
		*isNull = true;
		return (Datum) 0;
	}

	tupType = HeapTupleHeaderGetTypeId(tuple);
	tupTypmod = HeapTupleHeaderGetTypMod(tuple);
	tupDesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

	/*
	 * heap_getattr needs a HeapTuple not a bare HeapTupleHeader.  We set all
	 * the fields in the struct just in case user tries to inspect system
	 * columns.
	 */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = tuple;

	result = heap_getattr(&tmptup,
						  attrno,
						  tupDesc,
						  isNull);

	ReleaseTupleDesc(tupDesc);

	return result;
}

/*
 * Number of items in a tlist (including any resjunk items!)
 */
int
ExecTargetListLength(List *targetlist)
{
	/* This used to be more complex, but fjoins are dead */
	return list_length(targetlist);
}

/*
 * Number of items in a tlist, not including any resjunk items
 */
int
ExecCleanTargetListLength(List *targetlist)
{
	int			len = 0;
	ListCell   *tl;

	foreach(tl, targetlist)
	{
		TargetEntry *curTle = lfirst_node(TargetEntry, tl);

		if (!curTle->resjunk)
			len++;
	}
	return len;
}

/*
 * Return a relInfo's tuple slot for a trigger's OLD tuples.
 */
TupleTableSlot *
ExecGetTriggerOldSlot(EState *estate, ResultRelInfo *relInfo)
{
	if (relInfo->ri_TrigOldSlot == NULL)
	{
		Relation	rel = relInfo->ri_RelationDesc;
		MemoryContext oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

		relInfo->ri_TrigOldSlot =
			ExecInitExtraTupleSlot(estate,
								   RelationGetDescr(rel),
								   table_slot_callbacks(rel));

		MemoryContextSwitchTo(oldcontext);
	}

	return relInfo->ri_TrigOldSlot;
}

/*
 * Return a relInfo's tuple slot for a trigger's NEW tuples.
 */
TupleTableSlot *
ExecGetTriggerNewSlot(EState *estate, ResultRelInfo *relInfo)
{
	if (relInfo->ri_TrigNewSlot == NULL)
	{
		Relation	rel = relInfo->ri_RelationDesc;
		MemoryContext oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

		relInfo->ri_TrigNewSlot =
			ExecInitExtraTupleSlot(estate,
								   RelationGetDescr(rel),
								   table_slot_callbacks(rel));

		MemoryContextSwitchTo(oldcontext);
	}

	return relInfo->ri_TrigNewSlot;
}

/*
 * Return a relInfo's tuple slot for processing returning tuples.
 */
TupleTableSlot *
ExecGetReturningSlot(EState *estate, ResultRelInfo *relInfo)
{
	if (relInfo->ri_ReturningSlot == NULL)
	{
		Relation	rel = relInfo->ri_RelationDesc;
		MemoryContext oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

		relInfo->ri_ReturningSlot =
			ExecInitExtraTupleSlot(estate,
								   RelationGetDescr(rel),
								   table_slot_callbacks(rel));

		MemoryContextSwitchTo(oldcontext);
	}

	return relInfo->ri_ReturningSlot;
}

/*
 * Return the map needed to convert given child result relation's tuples to
 * the rowtype of the query's main target ("root") relation.  Note that a
 * NULL result is valid and means that no conversion is needed.
 */
TupleConversionMap *
ExecGetChildToRootMap(ResultRelInfo *resultRelInfo)
{
	/* If we didn't already do so, compute the map for this child. */
	if (!resultRelInfo->ri_ChildToRootMapValid)
	{
		ResultRelInfo *rootRelInfo = resultRelInfo->ri_RootResultRelInfo;

		if (rootRelInfo)
			resultRelInfo->ri_ChildToRootMap =
				convert_tuples_by_name(RelationGetDescr(resultRelInfo->ri_RelationDesc),
									   RelationGetDescr(rootRelInfo->ri_RelationDesc));
		else					/* this isn't a child result rel */
			resultRelInfo->ri_ChildToRootMap = NULL;

		resultRelInfo->ri_ChildToRootMapValid = true;
	}

	return resultRelInfo->ri_ChildToRootMap;
}

/*
 * Returns the map needed to convert given root result relation's tuples to
 * the rowtype of the given child relation.  Note that a NULL result is valid
 * and means that no conversion is needed.
 */
TupleConversionMap *
ExecGetRootToChildMap(ResultRelInfo *resultRelInfo, EState *estate)
{
	/* Mustn't get called for a non-child result relation. */
	Assert(resultRelInfo->ri_RootResultRelInfo);

	/* If we didn't already do so, compute the map for this child. */
	if (!resultRelInfo->ri_RootToChildMapValid)
	{
		ResultRelInfo *rootRelInfo = resultRelInfo->ri_RootResultRelInfo;
		TupleDesc	indesc = RelationGetDescr(rootRelInfo->ri_RelationDesc);
		TupleDesc	outdesc = RelationGetDescr(resultRelInfo->ri_RelationDesc);
		Relation	childrel = resultRelInfo->ri_RelationDesc;
		AttrMap    *attrMap;
		MemoryContext oldcontext;

		/*
		 * When this child table is not a partition (!relispartition), it may
		 * have columns that are not present in the root table, which we ask
		 * to ignore by passing true for missing_ok.
		 */
		oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);
		attrMap = build_attrmap_by_name_if_req(indesc, outdesc,
											   !childrel->rd_rel->relispartition);
		if (attrMap)
			resultRelInfo->ri_RootToChildMap =
				convert_tuples_by_name_attrmap(indesc, outdesc, attrMap);
		MemoryContextSwitchTo(oldcontext);
		resultRelInfo->ri_RootToChildMapValid = true;
	}

	return resultRelInfo->ri_RootToChildMap;
}

/* Return a bitmap representing columns being inserted */
Bitmapset *
ExecGetInsertedCols(ResultRelInfo *relinfo, EState *estate)
{
	RTEPermissionInfo *perminfo = GetResultRTEPermissionInfo(relinfo, estate);

	if (perminfo == NULL)
		return NULL;

	/* Map the columns to child's attribute numbers if needed. */
	if (relinfo->ri_RootResultRelInfo)
	{
		TupleConversionMap *map = ExecGetRootToChildMap(relinfo, estate);

		if (map)
			return execute_attr_map_cols(map->attrMap, perminfo->insertedCols);
	}

	return perminfo->insertedCols;
}

/* Return a bitmap representing columns being updated */
Bitmapset *
ExecGetUpdatedCols(ResultRelInfo *relinfo, EState *estate)
{
	RTEPermissionInfo *perminfo = GetResultRTEPermissionInfo(relinfo, estate);

	if (perminfo == NULL)
		return NULL;

	/* Map the columns to child's attribute numbers if needed. */
	if (relinfo->ri_RootResultRelInfo)
	{
		TupleConversionMap *map = ExecGetRootToChildMap(relinfo, estate);

		if (map)
			return execute_attr_map_cols(map->attrMap, perminfo->updatedCols);
	}

	return perminfo->updatedCols;
}

/* Return a bitmap representing generated columns being updated */
Bitmapset *
ExecGetExtraUpdatedCols(ResultRelInfo *relinfo, EState *estate)
{
	/* Compute the info if we didn't already */
	if (relinfo->ri_GeneratedExprsU == NULL)
		ExecInitStoredGenerated(relinfo, estate, CMD_UPDATE);
	return relinfo->ri_extraUpdatedCols;
}

/*
 * Return columns being updated, including generated columns
 *
 * The bitmap is allocated in per-tuple memory context. It's up to the caller to
 * copy it into a different context with the appropriate lifespan, if needed.
 */
Bitmapset *
ExecGetAllUpdatedCols(ResultRelInfo *relinfo, EState *estate)
{
	Bitmapset  *ret;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

	ret = bms_union(ExecGetUpdatedCols(relinfo, estate),
					ExecGetExtraUpdatedCols(relinfo, estate));

	MemoryContextSwitchTo(oldcxt);

	return ret;
}

/*
 * GetResultRTEPermissionInfo
 *		Looks up RTEPermissionInfo for ExecGet*Cols() routines
 */
static RTEPermissionInfo *
GetResultRTEPermissionInfo(ResultRelInfo *relinfo, EState *estate)
{
	Index		rti;
	RangeTblEntry *rte;
	RTEPermissionInfo *perminfo = NULL;

	if (relinfo->ri_RootResultRelInfo)
	{
		/*
		 * For inheritance child result relations (a partition routing target
		 * of an INSERT or a child UPDATE target), this returns the root
		 * parent's RTE to fetch the RTEPermissionInfo because that's the only
		 * one that has one assigned.
		 */
		rti = relinfo->ri_RootResultRelInfo->ri_RangeTableIndex;
	}
	else if (relinfo->ri_RangeTableIndex != 0)
	{
		/*
		 * Non-child result relation should have their own RTEPermissionInfo.
		 */
		rti = relinfo->ri_RangeTableIndex;
	}
	else
	{
		/*
		 * The relation isn't in the range table and it isn't a partition
		 * routing target.  This ResultRelInfo must've been created only for
		 * firing triggers and the relation is not being inserted into.  (See
		 * ExecGetTriggerResultRel.)
		 */
		rti = 0;
	}

	if (rti > 0)
	{
		rte = exec_rt_fetch(rti, estate);
		perminfo = getRTEPermissionInfo(estate->es_rteperminfos, rte);
	}

	return perminfo;
}

/*
 * ExecGetResultRelCheckAsUser
 *		Returns the user to modify passed-in result relation as
 *
 * The user is chosen by looking up the relation's or, if a child table, its
 * root parent's RTEPermissionInfo.
 */
Oid
ExecGetResultRelCheckAsUser(ResultRelInfo *relInfo, EState *estate)
{
	RTEPermissionInfo *perminfo = GetResultRTEPermissionInfo(relInfo, estate);

	/* XXX - maybe ok to return GetUserId() in this case? */
	if (perminfo == NULL)
		elog(ERROR, "no RTEPermissionInfo found for result relation with OID %u",
			 RelationGetRelid(relInfo->ri_RelationDesc));

	return perminfo->checkAsUser ? perminfo->checkAsUser : GetUserId();
}
