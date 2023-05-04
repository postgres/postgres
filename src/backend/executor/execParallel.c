/*-------------------------------------------------------------------------
 *
 * execParallel.c
 *	  Support routines for parallel execution.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * This file contains routines that are intended to support setting up,
 * using, and tearing down a ParallelContext from within the PostgreSQL
 * executor.  The ParallelContext machinery will handle starting the
 * workers and ensuring that their state generally matches that of the
 * leader; see src/backend/access/transam/README.parallel for details.
 * However, we must save and restore relevant executor state, such as
 * any ParamListInfo associated with the query, buffer/WAL usage info, and
 * the actual plan to be passed down to the worker.
 *
 * IDENTIFICATION
 *	  src/backend/executor/execParallel.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/execParallel.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeAppend.h"
#include "executor/nodeBitmapHeapscan.h"
#include "executor/nodeCustom.h"
#include "executor/nodeForeignscan.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "executor/nodeIncrementalSort.h"
#include "executor/nodeIndexonlyscan.h"
#include "executor/nodeIndexscan.h"
#include "executor/nodeMemoize.h"
#include "executor/nodeSeqscan.h"
#include "executor/nodeSort.h"
#include "executor/nodeSubplan.h"
#include "executor/tqueue.h"
#include "jit/jit.h"
#include "nodes/nodeFuncs.h"
#include "pgstat.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/datum.h"
#include "utils/dsa.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

/*
 * Magic numbers for parallel executor communication.  We use constants
 * greater than any 32-bit integer here so that values < 2^32 can be used
 * by individual parallel nodes to store their own state.
 */
#define PARALLEL_KEY_EXECUTOR_FIXED		UINT64CONST(0xE000000000000001)
#define PARALLEL_KEY_PLANNEDSTMT		UINT64CONST(0xE000000000000002)
#define PARALLEL_KEY_PARAMLISTINFO		UINT64CONST(0xE000000000000003)
#define PARALLEL_KEY_BUFFER_USAGE		UINT64CONST(0xE000000000000004)
#define PARALLEL_KEY_TUPLE_QUEUE		UINT64CONST(0xE000000000000005)
#define PARALLEL_KEY_INSTRUMENTATION	UINT64CONST(0xE000000000000006)
#define PARALLEL_KEY_DSA				UINT64CONST(0xE000000000000007)
#define PARALLEL_KEY_QUERY_TEXT		UINT64CONST(0xE000000000000008)
#define PARALLEL_KEY_JIT_INSTRUMENTATION UINT64CONST(0xE000000000000009)
#define PARALLEL_KEY_WAL_USAGE			UINT64CONST(0xE00000000000000A)

#define PARALLEL_TUPLE_QUEUE_SIZE		65536

/*
 * Fixed-size random stuff that we need to pass to parallel workers.
 */
typedef struct FixedParallelExecutorState
{
	int64		tuples_needed;	/* tuple bound, see ExecSetTupleBound */
	dsa_pointer param_exec;
	int			eflags;
	int			jit_flags;
} FixedParallelExecutorState;

/*
 * DSM structure for accumulating per-PlanState instrumentation.
 *
 * instrument_options: Same meaning here as in instrument.c.
 *
 * instrument_offset: Offset, relative to the start of this structure,
 * of the first Instrumentation object.  This will depend on the length of
 * the plan_node_id array.
 *
 * num_workers: Number of workers.
 *
 * num_plan_nodes: Number of plan nodes.
 *
 * plan_node_id: Array of plan nodes for which we are gathering instrumentation
 * from parallel workers.  The length of this array is given by num_plan_nodes.
 */
struct SharedExecutorInstrumentation
{
	int			instrument_options;
	int			instrument_offset;
	int			num_workers;
	int			num_plan_nodes;
	int			plan_node_id[FLEXIBLE_ARRAY_MEMBER];
	/* array of num_plan_nodes * num_workers Instrumentation objects follows */
};
#define GetInstrumentationArray(sei) \
	(AssertVariableIsOfTypeMacro(sei, SharedExecutorInstrumentation *), \
	 (Instrumentation *) (((char *) sei) + sei->instrument_offset))

/* Context object for ExecParallelEstimate. */
typedef struct ExecParallelEstimateContext
{
	ParallelContext *pcxt;
	int			nnodes;
} ExecParallelEstimateContext;

/* Context object for ExecParallelInitializeDSM. */
typedef struct ExecParallelInitializeDSMContext
{
	ParallelContext *pcxt;
	SharedExecutorInstrumentation *instrumentation;
	int			nnodes;
} ExecParallelInitializeDSMContext;

/* Helper functions that run in the parallel leader. */
static char *ExecSerializePlan(Plan *plan, EState *estate);
static bool ExecParallelEstimate(PlanState *planstate,
								 ExecParallelEstimateContext *e);
static bool ExecParallelInitializeDSM(PlanState *planstate,
									  ExecParallelInitializeDSMContext *d);
static shm_mq_handle **ExecParallelSetupTupleQueues(ParallelContext *pcxt,
													bool reinitialize);
static bool ExecParallelReInitializeDSM(PlanState *planstate,
										ParallelContext *pcxt);
static bool ExecParallelRetrieveInstrumentation(PlanState *planstate,
												SharedExecutorInstrumentation *instrumentation);

/* Helper function that runs in the parallel worker. */
static DestReceiver *ExecParallelGetReceiver(dsm_segment *seg, shm_toc *toc);

/*
 * Create a serialized representation of the plan to be sent to each worker.
 */
static char *
ExecSerializePlan(Plan *plan, EState *estate)
{
	PlannedStmt *pstmt;
	ListCell   *lc;

	/* We can't scribble on the original plan, so make a copy. */
	plan = copyObject(plan);

	/*
	 * The worker will start its own copy of the executor, and that copy will
	 * insert a junk filter if the toplevel node has any resjunk entries. We
	 * don't want that to happen, because while resjunk columns shouldn't be
	 * sent back to the user, here the tuples are coming back to another
	 * backend which may very well need them.  So mutate the target list
	 * accordingly.  This is sort of a hack; there might be better ways to do
	 * this...
	 */
	foreach(lc, plan->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		tle->resjunk = false;
	}

	/*
	 * Create a dummy PlannedStmt.  Most of the fields don't need to be valid
	 * for our purposes, but the worker will need at least a minimal
	 * PlannedStmt to start the executor.
	 */
	pstmt = makeNode(PlannedStmt);
	pstmt->commandType = CMD_SELECT;
	pstmt->queryId = pgstat_get_my_query_id();
	pstmt->hasReturning = false;
	pstmt->hasModifyingCTE = false;
	pstmt->canSetTag = true;
	pstmt->transientPlan = false;
	pstmt->dependsOnRole = false;
	pstmt->parallelModeNeeded = false;
	pstmt->planTree = plan;
	pstmt->rtable = estate->es_range_table;
	pstmt->permInfos = estate->es_rteperminfos;
	pstmt->resultRelations = NIL;
	pstmt->appendRelations = NIL;

	/*
	 * Transfer only parallel-safe subplans, leaving a NULL "hole" in the list
	 * for unsafe ones (so that the list indexes of the safe ones are
	 * preserved).  This positively ensures that the worker won't try to run,
	 * or even do ExecInitNode on, an unsafe subplan.  That's important to
	 * protect, eg, non-parallel-aware FDWs from getting into trouble.
	 */
	pstmt->subplans = NIL;
	foreach(lc, estate->es_plannedstmt->subplans)
	{
		Plan	   *subplan = (Plan *) lfirst(lc);

		if (subplan && !subplan->parallel_safe)
			subplan = NULL;
		pstmt->subplans = lappend(pstmt->subplans, subplan);
	}

	pstmt->rewindPlanIDs = NULL;
	pstmt->rowMarks = NIL;
	pstmt->relationOids = NIL;
	pstmt->invalItems = NIL;	/* workers can't replan anyway... */
	pstmt->paramExecTypes = estate->es_plannedstmt->paramExecTypes;
	pstmt->utilityStmt = NULL;
	pstmt->stmt_location = -1;
	pstmt->stmt_len = -1;

	/* Return serialized copy of our dummy PlannedStmt. */
	return nodeToString(pstmt);
}

/*
 * Parallel-aware plan nodes (and occasionally others) may need some state
 * which is shared across all parallel workers.  Before we size the DSM, give
 * them a chance to call shm_toc_estimate_chunk or shm_toc_estimate_keys on
 * &pcxt->estimator.
 *
 * While we're at it, count the number of PlanState nodes in the tree, so
 * we know how many Instrumentation structures we need.
 */
static bool
ExecParallelEstimate(PlanState *planstate, ExecParallelEstimateContext *e)
{
	if (planstate == NULL)
		return false;

	/* Count this node. */
	e->nnodes++;

	switch (nodeTag(planstate))
	{
		case T_SeqScanState:
			if (planstate->plan->parallel_aware)
				ExecSeqScanEstimate((SeqScanState *) planstate,
									e->pcxt);
			break;
		case T_IndexScanState:
			if (planstate->plan->parallel_aware)
				ExecIndexScanEstimate((IndexScanState *) planstate,
									  e->pcxt);
			break;
		case T_IndexOnlyScanState:
			if (planstate->plan->parallel_aware)
				ExecIndexOnlyScanEstimate((IndexOnlyScanState *) planstate,
										  e->pcxt);
			break;
		case T_ForeignScanState:
			if (planstate->plan->parallel_aware)
				ExecForeignScanEstimate((ForeignScanState *) planstate,
										e->pcxt);
			break;
		case T_AppendState:
			if (planstate->plan->parallel_aware)
				ExecAppendEstimate((AppendState *) planstate,
								   e->pcxt);
			break;
		case T_CustomScanState:
			if (planstate->plan->parallel_aware)
				ExecCustomScanEstimate((CustomScanState *) planstate,
									   e->pcxt);
			break;
		case T_BitmapHeapScanState:
			if (planstate->plan->parallel_aware)
				ExecBitmapHeapEstimate((BitmapHeapScanState *) planstate,
									   e->pcxt);
			break;
		case T_HashJoinState:
			if (planstate->plan->parallel_aware)
				ExecHashJoinEstimate((HashJoinState *) planstate,
									 e->pcxt);
			break;
		case T_HashState:
			/* even when not parallel-aware, for EXPLAIN ANALYZE */
			ExecHashEstimate((HashState *) planstate, e->pcxt);
			break;
		case T_SortState:
			/* even when not parallel-aware, for EXPLAIN ANALYZE */
			ExecSortEstimate((SortState *) planstate, e->pcxt);
			break;
		case T_IncrementalSortState:
			/* even when not parallel-aware, for EXPLAIN ANALYZE */
			ExecIncrementalSortEstimate((IncrementalSortState *) planstate, e->pcxt);
			break;
		case T_AggState:
			/* even when not parallel-aware, for EXPLAIN ANALYZE */
			ExecAggEstimate((AggState *) planstate, e->pcxt);
			break;
		case T_MemoizeState:
			/* even when not parallel-aware, for EXPLAIN ANALYZE */
			ExecMemoizeEstimate((MemoizeState *) planstate, e->pcxt);
			break;
		default:
			break;
	}

	return planstate_tree_walker(planstate, ExecParallelEstimate, e);
}

/*
 * Estimate the amount of space required to serialize the indicated parameters.
 */
static Size
EstimateParamExecSpace(EState *estate, Bitmapset *params)
{
	int			paramid;
	Size		sz = sizeof(int);

	paramid = -1;
	while ((paramid = bms_next_member(params, paramid)) >= 0)
	{
		Oid			typeOid;
		int16		typLen;
		bool		typByVal;
		ParamExecData *prm;

		prm = &(estate->es_param_exec_vals[paramid]);
		typeOid = list_nth_oid(estate->es_plannedstmt->paramExecTypes,
							   paramid);

		sz = add_size(sz, sizeof(int)); /* space for paramid */

		/* space for datum/isnull */
		if (OidIsValid(typeOid))
			get_typlenbyval(typeOid, &typLen, &typByVal);
		else
		{
			/* If no type OID, assume by-value, like copyParamList does. */
			typLen = sizeof(Datum);
			typByVal = true;
		}
		sz = add_size(sz,
					  datumEstimateSpace(prm->value, prm->isnull,
										 typByVal, typLen));
	}
	return sz;
}

/*
 * Serialize specified PARAM_EXEC parameters.
 *
 * We write the number of parameters first, as a 4-byte integer, and then
 * write details for each parameter in turn.  The details for each parameter
 * consist of a 4-byte paramid (location of param in execution time internal
 * parameter array) and then the datum as serialized by datumSerialize().
 */
static dsa_pointer
SerializeParamExecParams(EState *estate, Bitmapset *params, dsa_area *area)
{
	Size		size;
	int			nparams;
	int			paramid;
	ParamExecData *prm;
	dsa_pointer handle;
	char	   *start_address;

	/* Allocate enough space for the current parameter values. */
	size = EstimateParamExecSpace(estate, params);
	handle = dsa_allocate(area, size);
	start_address = dsa_get_address(area, handle);

	/* First write the number of parameters as a 4-byte integer. */
	nparams = bms_num_members(params);
	memcpy(start_address, &nparams, sizeof(int));
	start_address += sizeof(int);

	/* Write details for each parameter in turn. */
	paramid = -1;
	while ((paramid = bms_next_member(params, paramid)) >= 0)
	{
		Oid			typeOid;
		int16		typLen;
		bool		typByVal;

		prm = &(estate->es_param_exec_vals[paramid]);
		typeOid = list_nth_oid(estate->es_plannedstmt->paramExecTypes,
							   paramid);

		/* Write paramid. */
		memcpy(start_address, &paramid, sizeof(int));
		start_address += sizeof(int);

		/* Write datum/isnull */
		if (OidIsValid(typeOid))
			get_typlenbyval(typeOid, &typLen, &typByVal);
		else
		{
			/* If no type OID, assume by-value, like copyParamList does. */
			typLen = sizeof(Datum);
			typByVal = true;
		}
		datumSerialize(prm->value, prm->isnull, typByVal, typLen,
					   &start_address);
	}

	return handle;
}

/*
 * Restore specified PARAM_EXEC parameters.
 */
static void
RestoreParamExecParams(char *start_address, EState *estate)
{
	int			nparams;
	int			i;
	int			paramid;

	memcpy(&nparams, start_address, sizeof(int));
	start_address += sizeof(int);

	for (i = 0; i < nparams; i++)
	{
		ParamExecData *prm;

		/* Read paramid */
		memcpy(&paramid, start_address, sizeof(int));
		start_address += sizeof(int);
		prm = &(estate->es_param_exec_vals[paramid]);

		/* Read datum/isnull. */
		prm->value = datumRestore(&start_address, &prm->isnull);
		prm->execPlan = NULL;
	}
}

/*
 * Initialize the dynamic shared memory segment that will be used to control
 * parallel execution.
 */
static bool
ExecParallelInitializeDSM(PlanState *planstate,
						  ExecParallelInitializeDSMContext *d)
{
	if (planstate == NULL)
		return false;

	/* If instrumentation is enabled, initialize slot for this node. */
	if (d->instrumentation != NULL)
		d->instrumentation->plan_node_id[d->nnodes] =
			planstate->plan->plan_node_id;

	/* Count this node. */
	d->nnodes++;

	/*
	 * Call initializers for DSM-using plan nodes.
	 *
	 * Most plan nodes won't do anything here, but plan nodes that allocated
	 * DSM may need to initialize shared state in the DSM before parallel
	 * workers are launched.  They can allocate the space they previously
	 * estimated using shm_toc_allocate, and add the keys they previously
	 * estimated using shm_toc_insert, in each case targeting pcxt->toc.
	 */
	switch (nodeTag(planstate))
	{
		case T_SeqScanState:
			if (planstate->plan->parallel_aware)
				ExecSeqScanInitializeDSM((SeqScanState *) planstate,
										 d->pcxt);
			break;
		case T_IndexScanState:
			if (planstate->plan->parallel_aware)
				ExecIndexScanInitializeDSM((IndexScanState *) planstate,
										   d->pcxt);
			break;
		case T_IndexOnlyScanState:
			if (planstate->plan->parallel_aware)
				ExecIndexOnlyScanInitializeDSM((IndexOnlyScanState *) planstate,
											   d->pcxt);
			break;
		case T_ForeignScanState:
			if (planstate->plan->parallel_aware)
				ExecForeignScanInitializeDSM((ForeignScanState *) planstate,
											 d->pcxt);
			break;
		case T_AppendState:
			if (planstate->plan->parallel_aware)
				ExecAppendInitializeDSM((AppendState *) planstate,
										d->pcxt);
			break;
		case T_CustomScanState:
			if (planstate->plan->parallel_aware)
				ExecCustomScanInitializeDSM((CustomScanState *) planstate,
											d->pcxt);
			break;
		case T_BitmapHeapScanState:
			if (planstate->plan->parallel_aware)
				ExecBitmapHeapInitializeDSM((BitmapHeapScanState *) planstate,
											d->pcxt);
			break;
		case T_HashJoinState:
			if (planstate->plan->parallel_aware)
				ExecHashJoinInitializeDSM((HashJoinState *) planstate,
										  d->pcxt);
			break;
		case T_HashState:
			/* even when not parallel-aware, for EXPLAIN ANALYZE */
			ExecHashInitializeDSM((HashState *) planstate, d->pcxt);
			break;
		case T_SortState:
			/* even when not parallel-aware, for EXPLAIN ANALYZE */
			ExecSortInitializeDSM((SortState *) planstate, d->pcxt);
			break;
		case T_IncrementalSortState:
			/* even when not parallel-aware, for EXPLAIN ANALYZE */
			ExecIncrementalSortInitializeDSM((IncrementalSortState *) planstate, d->pcxt);
			break;
		case T_AggState:
			/* even when not parallel-aware, for EXPLAIN ANALYZE */
			ExecAggInitializeDSM((AggState *) planstate, d->pcxt);
			break;
		case T_MemoizeState:
			/* even when not parallel-aware, for EXPLAIN ANALYZE */
			ExecMemoizeInitializeDSM((MemoizeState *) planstate, d->pcxt);
			break;
		default:
			break;
	}

	return planstate_tree_walker(planstate, ExecParallelInitializeDSM, d);
}

/*
 * It sets up the response queues for backend workers to return tuples
 * to the main backend and start the workers.
 */
static shm_mq_handle **
ExecParallelSetupTupleQueues(ParallelContext *pcxt, bool reinitialize)
{
	shm_mq_handle **responseq;
	char	   *tqueuespace;
	int			i;

	/* Skip this if no workers. */
	if (pcxt->nworkers == 0)
		return NULL;

	/* Allocate memory for shared memory queue handles. */
	responseq = (shm_mq_handle **)
		palloc(pcxt->nworkers * sizeof(shm_mq_handle *));

	/*
	 * If not reinitializing, allocate space from the DSM for the queues;
	 * otherwise, find the already allocated space.
	 */
	if (!reinitialize)
		tqueuespace =
			shm_toc_allocate(pcxt->toc,
							 mul_size(PARALLEL_TUPLE_QUEUE_SIZE,
									  pcxt->nworkers));
	else
		tqueuespace = shm_toc_lookup(pcxt->toc, PARALLEL_KEY_TUPLE_QUEUE, false);

	/* Create the queues, and become the receiver for each. */
	for (i = 0; i < pcxt->nworkers; ++i)
	{
		shm_mq	   *mq;

		mq = shm_mq_create(tqueuespace +
						   ((Size) i) * PARALLEL_TUPLE_QUEUE_SIZE,
						   (Size) PARALLEL_TUPLE_QUEUE_SIZE);

		shm_mq_set_receiver(mq, MyProc);
		responseq[i] = shm_mq_attach(mq, pcxt->seg, NULL);
	}

	/* Add array of queues to shm_toc, so others can find it. */
	if (!reinitialize)
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_TUPLE_QUEUE, tqueuespace);

	/* Return array of handles. */
	return responseq;
}

/*
 * Sets up the required infrastructure for backend workers to perform
 * execution and return results to the main backend.
 */
ParallelExecutorInfo *
ExecInitParallelPlan(PlanState *planstate, EState *estate,
					 Bitmapset *sendParams, int nworkers,
					 int64 tuples_needed)
{
	ParallelExecutorInfo *pei;
	ParallelContext *pcxt;
	ExecParallelEstimateContext e;
	ExecParallelInitializeDSMContext d;
	FixedParallelExecutorState *fpes;
	char	   *pstmt_data;
	char	   *pstmt_space;
	char	   *paramlistinfo_space;
	BufferUsage *bufusage_space;
	WalUsage   *walusage_space;
	SharedExecutorInstrumentation *instrumentation = NULL;
	SharedJitInstrumentation *jit_instrumentation = NULL;
	int			pstmt_len;
	int			paramlistinfo_len;
	int			instrumentation_len = 0;
	int			jit_instrumentation_len = 0;
	int			instrument_offset = 0;
	Size		dsa_minsize = dsa_minimum_size();
	char	   *query_string;
	int			query_len;

	/*
	 * Force any initplan outputs that we're going to pass to workers to be
	 * evaluated, if they weren't already.
	 *
	 * For simplicity, we use the EState's per-output-tuple ExprContext here.
	 * That risks intra-query memory leakage, since we might pass through here
	 * many times before that ExprContext gets reset; but ExecSetParamPlan
	 * doesn't normally leak any memory in the context (see its comments), so
	 * it doesn't seem worth complicating this function's API to pass it a
	 * shorter-lived ExprContext.  This might need to change someday.
	 */
	ExecSetParamPlanMulti(sendParams, GetPerTupleExprContext(estate));

	/* Allocate object for return value. */
	pei = palloc0(sizeof(ParallelExecutorInfo));
	pei->finished = false;
	pei->planstate = planstate;

	/* Fix up and serialize plan to be sent to workers. */
	pstmt_data = ExecSerializePlan(planstate->plan, estate);

	/* Create a parallel context. */
	pcxt = CreateParallelContext("postgres", "ParallelQueryMain", nworkers);
	pei->pcxt = pcxt;

	/*
	 * Before telling the parallel context to create a dynamic shared memory
	 * segment, we need to figure out how big it should be.  Estimate space
	 * for the various things we need to store.
	 */

	/* Estimate space for fixed-size state. */
	shm_toc_estimate_chunk(&pcxt->estimator,
						   sizeof(FixedParallelExecutorState));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* Estimate space for query text. */
	query_len = strlen(estate->es_sourceText);
	shm_toc_estimate_chunk(&pcxt->estimator, query_len + 1);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* Estimate space for serialized PlannedStmt. */
	pstmt_len = strlen(pstmt_data) + 1;
	shm_toc_estimate_chunk(&pcxt->estimator, pstmt_len);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* Estimate space for serialized ParamListInfo. */
	paramlistinfo_len = EstimateParamListSpace(estate->es_param_list_info);
	shm_toc_estimate_chunk(&pcxt->estimator, paramlistinfo_len);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/*
	 * Estimate space for BufferUsage.
	 *
	 * If EXPLAIN is not in use and there are no extensions loaded that care,
	 * we could skip this.  But we have no way of knowing whether anyone's
	 * looking at pgBufferUsage, so do it unconditionally.
	 */
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/*
	 * Same thing for WalUsage.
	 */
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* Estimate space for tuple queues. */
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(PARALLEL_TUPLE_QUEUE_SIZE, pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/*
	 * Give parallel-aware nodes a chance to add to the estimates, and get a
	 * count of how many PlanState nodes there are.
	 */
	e.pcxt = pcxt;
	e.nnodes = 0;
	ExecParallelEstimate(planstate, &e);

	/* Estimate space for instrumentation, if required. */
	if (estate->es_instrument)
	{
		instrumentation_len =
			offsetof(SharedExecutorInstrumentation, plan_node_id) +
			sizeof(int) * e.nnodes;
		instrumentation_len = MAXALIGN(instrumentation_len);
		instrument_offset = instrumentation_len;
		instrumentation_len +=
			mul_size(sizeof(Instrumentation),
					 mul_size(e.nnodes, nworkers));
		shm_toc_estimate_chunk(&pcxt->estimator, instrumentation_len);
		shm_toc_estimate_keys(&pcxt->estimator, 1);

		/* Estimate space for JIT instrumentation, if required. */
		if (estate->es_jit_flags != PGJIT_NONE)
		{
			jit_instrumentation_len =
				offsetof(SharedJitInstrumentation, jit_instr) +
				sizeof(JitInstrumentation) * nworkers;
			shm_toc_estimate_chunk(&pcxt->estimator, jit_instrumentation_len);
			shm_toc_estimate_keys(&pcxt->estimator, 1);
		}
	}

	/* Estimate space for DSA area. */
	shm_toc_estimate_chunk(&pcxt->estimator, dsa_minsize);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* Everyone's had a chance to ask for space, so now create the DSM. */
	InitializeParallelDSM(pcxt);

	/*
	 * OK, now we have a dynamic shared memory segment, and it should be big
	 * enough to store all of the data we estimated we would want to put into
	 * it, plus whatever general stuff (not specifically executor-related) the
	 * ParallelContext itself needs to store there.  None of the space we
	 * asked for has been allocated or initialized yet, though, so do that.
	 */

	/* Store fixed-size state. */
	fpes = shm_toc_allocate(pcxt->toc, sizeof(FixedParallelExecutorState));
	fpes->tuples_needed = tuples_needed;
	fpes->param_exec = InvalidDsaPointer;
	fpes->eflags = estate->es_top_eflags;
	fpes->jit_flags = estate->es_jit_flags;
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_EXECUTOR_FIXED, fpes);

	/* Store query string */
	query_string = shm_toc_allocate(pcxt->toc, query_len + 1);
	memcpy(query_string, estate->es_sourceText, query_len + 1);
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_QUERY_TEXT, query_string);

	/* Store serialized PlannedStmt. */
	pstmt_space = shm_toc_allocate(pcxt->toc, pstmt_len);
	memcpy(pstmt_space, pstmt_data, pstmt_len);
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_PLANNEDSTMT, pstmt_space);

	/* Store serialized ParamListInfo. */
	paramlistinfo_space = shm_toc_allocate(pcxt->toc, paramlistinfo_len);
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_PARAMLISTINFO, paramlistinfo_space);
	SerializeParamList(estate->es_param_list_info, &paramlistinfo_space);

	/* Allocate space for each worker's BufferUsage; no need to initialize. */
	bufusage_space = shm_toc_allocate(pcxt->toc,
									  mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_BUFFER_USAGE, bufusage_space);
	pei->buffer_usage = bufusage_space;

	/* Same for WalUsage. */
	walusage_space = shm_toc_allocate(pcxt->toc,
									  mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_WAL_USAGE, walusage_space);
	pei->wal_usage = walusage_space;

	/* Set up the tuple queues that the workers will write into. */
	pei->tqueue = ExecParallelSetupTupleQueues(pcxt, false);

	/* We don't need the TupleQueueReaders yet, though. */
	pei->reader = NULL;

	/*
	 * If instrumentation options were supplied, allocate space for the data.
	 * It only gets partially initialized here; the rest happens during
	 * ExecParallelInitializeDSM.
	 */
	if (estate->es_instrument)
	{
		Instrumentation *instrument;
		int			i;

		instrumentation = shm_toc_allocate(pcxt->toc, instrumentation_len);
		instrumentation->instrument_options = estate->es_instrument;
		instrumentation->instrument_offset = instrument_offset;
		instrumentation->num_workers = nworkers;
		instrumentation->num_plan_nodes = e.nnodes;
		instrument = GetInstrumentationArray(instrumentation);
		for (i = 0; i < nworkers * e.nnodes; ++i)
			InstrInit(&instrument[i], estate->es_instrument);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_INSTRUMENTATION,
					   instrumentation);
		pei->instrumentation = instrumentation;

		if (estate->es_jit_flags != PGJIT_NONE)
		{
			jit_instrumentation = shm_toc_allocate(pcxt->toc,
												   jit_instrumentation_len);
			jit_instrumentation->num_workers = nworkers;
			memset(jit_instrumentation->jit_instr, 0,
				   sizeof(JitInstrumentation) * nworkers);
			shm_toc_insert(pcxt->toc, PARALLEL_KEY_JIT_INSTRUMENTATION,
						   jit_instrumentation);
			pei->jit_instrumentation = jit_instrumentation;
		}
	}

	/*
	 * Create a DSA area that can be used by the leader and all workers.
	 * (However, if we failed to create a DSM and are using private memory
	 * instead, then skip this.)
	 */
	if (pcxt->seg != NULL)
	{
		char	   *area_space;

		area_space = shm_toc_allocate(pcxt->toc, dsa_minsize);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_DSA, area_space);
		pei->area = dsa_create_in_place(area_space, dsa_minsize,
										LWTRANCHE_PARALLEL_QUERY_DSA,
										pcxt->seg);

		/*
		 * Serialize parameters, if any, using DSA storage.  We don't dare use
		 * the main parallel query DSM for this because we might relaunch
		 * workers after the values have changed (and thus the amount of
		 * storage required has changed).
		 */
		if (!bms_is_empty(sendParams))
		{
			pei->param_exec = SerializeParamExecParams(estate, sendParams,
													   pei->area);
			fpes->param_exec = pei->param_exec;
		}
	}

	/*
	 * Give parallel-aware nodes a chance to initialize their shared data.
	 * This also initializes the elements of instrumentation->ps_instrument,
	 * if it exists.
	 */
	d.pcxt = pcxt;
	d.instrumentation = instrumentation;
	d.nnodes = 0;

	/* Install our DSA area while initializing the plan. */
	estate->es_query_dsa = pei->area;
	ExecParallelInitializeDSM(planstate, &d);
	estate->es_query_dsa = NULL;

	/*
	 * Make sure that the world hasn't shifted under our feet.  This could
	 * probably just be an Assert(), but let's be conservative for now.
	 */
	if (e.nnodes != d.nnodes)
		elog(ERROR, "inconsistent count of PlanState nodes");

	/* OK, we're ready to rock and roll. */
	return pei;
}

/*
 * Set up tuple queue readers to read the results of a parallel subplan.
 *
 * This is separate from ExecInitParallelPlan() because we can launch the
 * worker processes and let them start doing something before we do this.
 */
void
ExecParallelCreateReaders(ParallelExecutorInfo *pei)
{
	int			nworkers = pei->pcxt->nworkers_launched;
	int			i;

	Assert(pei->reader == NULL);

	if (nworkers > 0)
	{
		pei->reader = (TupleQueueReader **)
			palloc(nworkers * sizeof(TupleQueueReader *));

		for (i = 0; i < nworkers; i++)
		{
			shm_mq_set_handle(pei->tqueue[i],
							  pei->pcxt->worker[i].bgwhandle);
			pei->reader[i] = CreateTupleQueueReader(pei->tqueue[i]);
		}
	}
}

/*
 * Re-initialize the parallel executor shared memory state before launching
 * a fresh batch of workers.
 */
void
ExecParallelReinitialize(PlanState *planstate,
						 ParallelExecutorInfo *pei,
						 Bitmapset *sendParams)
{
	EState	   *estate = planstate->state;
	FixedParallelExecutorState *fpes;

	/* Old workers must already be shut down */
	Assert(pei->finished);

	/*
	 * Force any initplan outputs that we're going to pass to workers to be
	 * evaluated, if they weren't already (see comments in
	 * ExecInitParallelPlan).
	 */
	ExecSetParamPlanMulti(sendParams, GetPerTupleExprContext(estate));

	ReinitializeParallelDSM(pei->pcxt);
	pei->tqueue = ExecParallelSetupTupleQueues(pei->pcxt, true);
	pei->reader = NULL;
	pei->finished = false;

	fpes = shm_toc_lookup(pei->pcxt->toc, PARALLEL_KEY_EXECUTOR_FIXED, false);

	/* Free any serialized parameters from the last round. */
	if (DsaPointerIsValid(fpes->param_exec))
	{
		dsa_free(pei->area, fpes->param_exec);
		fpes->param_exec = InvalidDsaPointer;
	}

	/* Serialize current parameter values if required. */
	if (!bms_is_empty(sendParams))
	{
		pei->param_exec = SerializeParamExecParams(estate, sendParams,
												   pei->area);
		fpes->param_exec = pei->param_exec;
	}

	/* Traverse plan tree and let each child node reset associated state. */
	estate->es_query_dsa = pei->area;
	ExecParallelReInitializeDSM(planstate, pei->pcxt);
	estate->es_query_dsa = NULL;
}

/*
 * Traverse plan tree to reinitialize per-node dynamic shared memory state
 */
static bool
ExecParallelReInitializeDSM(PlanState *planstate,
							ParallelContext *pcxt)
{
	if (planstate == NULL)
		return false;

	/*
	 * Call reinitializers for DSM-using plan nodes.
	 */
	switch (nodeTag(planstate))
	{
		case T_SeqScanState:
			if (planstate->plan->parallel_aware)
				ExecSeqScanReInitializeDSM((SeqScanState *) planstate,
										   pcxt);
			break;
		case T_IndexScanState:
			if (planstate->plan->parallel_aware)
				ExecIndexScanReInitializeDSM((IndexScanState *) planstate,
											 pcxt);
			break;
		case T_IndexOnlyScanState:
			if (planstate->plan->parallel_aware)
				ExecIndexOnlyScanReInitializeDSM((IndexOnlyScanState *) planstate,
												 pcxt);
			break;
		case T_ForeignScanState:
			if (planstate->plan->parallel_aware)
				ExecForeignScanReInitializeDSM((ForeignScanState *) planstate,
											   pcxt);
			break;
		case T_AppendState:
			if (planstate->plan->parallel_aware)
				ExecAppendReInitializeDSM((AppendState *) planstate, pcxt);
			break;
		case T_CustomScanState:
			if (planstate->plan->parallel_aware)
				ExecCustomScanReInitializeDSM((CustomScanState *) planstate,
											  pcxt);
			break;
		case T_BitmapHeapScanState:
			if (planstate->plan->parallel_aware)
				ExecBitmapHeapReInitializeDSM((BitmapHeapScanState *) planstate,
											  pcxt);
			break;
		case T_HashJoinState:
			if (planstate->plan->parallel_aware)
				ExecHashJoinReInitializeDSM((HashJoinState *) planstate,
											pcxt);
			break;
		case T_HashState:
		case T_SortState:
		case T_IncrementalSortState:
		case T_MemoizeState:
			/* these nodes have DSM state, but no reinitialization is required */
			break;

		default:
			break;
	}

	return planstate_tree_walker(planstate, ExecParallelReInitializeDSM, pcxt);
}

/*
 * Copy instrumentation information about this node and its descendants from
 * dynamic shared memory.
 */
static bool
ExecParallelRetrieveInstrumentation(PlanState *planstate,
									SharedExecutorInstrumentation *instrumentation)
{
	Instrumentation *instrument;
	int			i;
	int			n;
	int			ibytes;
	int			plan_node_id = planstate->plan->plan_node_id;
	MemoryContext oldcontext;

	/* Find the instrumentation for this node. */
	for (i = 0; i < instrumentation->num_plan_nodes; ++i)
		if (instrumentation->plan_node_id[i] == plan_node_id)
			break;
	if (i >= instrumentation->num_plan_nodes)
		elog(ERROR, "plan node %d not found", plan_node_id);

	/* Accumulate the statistics from all workers. */
	instrument = GetInstrumentationArray(instrumentation);
	instrument += i * instrumentation->num_workers;
	for (n = 0; n < instrumentation->num_workers; ++n)
		InstrAggNode(planstate->instrument, &instrument[n]);

	/*
	 * Also store the per-worker detail.
	 *
	 * Worker instrumentation should be allocated in the same context as the
	 * regular instrumentation information, which is the per-query context.
	 * Switch into per-query memory context.
	 */
	oldcontext = MemoryContextSwitchTo(planstate->state->es_query_cxt);
	ibytes = mul_size(instrumentation->num_workers, sizeof(Instrumentation));
	planstate->worker_instrument =
		palloc(ibytes + offsetof(WorkerInstrumentation, instrument));
	MemoryContextSwitchTo(oldcontext);

	planstate->worker_instrument->num_workers = instrumentation->num_workers;
	memcpy(&planstate->worker_instrument->instrument, instrument, ibytes);

	/* Perform any node-type-specific work that needs to be done. */
	switch (nodeTag(planstate))
	{
		case T_SortState:
			ExecSortRetrieveInstrumentation((SortState *) planstate);
			break;
		case T_IncrementalSortState:
			ExecIncrementalSortRetrieveInstrumentation((IncrementalSortState *) planstate);
			break;
		case T_HashState:
			ExecHashRetrieveInstrumentation((HashState *) planstate);
			break;
		case T_AggState:
			ExecAggRetrieveInstrumentation((AggState *) planstate);
			break;
		case T_MemoizeState:
			ExecMemoizeRetrieveInstrumentation((MemoizeState *) planstate);
			break;
		default:
			break;
	}

	return planstate_tree_walker(planstate, ExecParallelRetrieveInstrumentation,
								 instrumentation);
}

/*
 * Add up the workers' JIT instrumentation from dynamic shared memory.
 */
static void
ExecParallelRetrieveJitInstrumentation(PlanState *planstate,
									   SharedJitInstrumentation *shared_jit)
{
	JitInstrumentation *combined;
	int			ibytes;

	int			n;

	/*
	 * Accumulate worker JIT instrumentation into the combined JIT
	 * instrumentation, allocating it if required.
	 */
	if (!planstate->state->es_jit_worker_instr)
		planstate->state->es_jit_worker_instr =
			MemoryContextAllocZero(planstate->state->es_query_cxt, sizeof(JitInstrumentation));
	combined = planstate->state->es_jit_worker_instr;

	/* Accumulate all the workers' instrumentations. */
	for (n = 0; n < shared_jit->num_workers; ++n)
		InstrJitAgg(combined, &shared_jit->jit_instr[n]);

	/*
	 * Store the per-worker detail.
	 *
	 * Similar to ExecParallelRetrieveInstrumentation(), allocate the
	 * instrumentation in per-query context.
	 */
	ibytes = offsetof(SharedJitInstrumentation, jit_instr)
		+ mul_size(shared_jit->num_workers, sizeof(JitInstrumentation));
	planstate->worker_jit_instrument =
		MemoryContextAlloc(planstate->state->es_query_cxt, ibytes);

	memcpy(planstate->worker_jit_instrument, shared_jit, ibytes);
}

/*
 * Finish parallel execution.  We wait for parallel workers to finish, and
 * accumulate their buffer/WAL usage.
 */
void
ExecParallelFinish(ParallelExecutorInfo *pei)
{
	int			nworkers = pei->pcxt->nworkers_launched;
	int			i;

	/* Make this be a no-op if called twice in a row. */
	if (pei->finished)
		return;

	/*
	 * Detach from tuple queues ASAP, so that any still-active workers will
	 * notice that no further results are wanted.
	 */
	if (pei->tqueue != NULL)
	{
		for (i = 0; i < nworkers; i++)
			shm_mq_detach(pei->tqueue[i]);
		pfree(pei->tqueue);
		pei->tqueue = NULL;
	}

	/*
	 * While we're waiting for the workers to finish, let's get rid of the
	 * tuple queue readers.  (Any other local cleanup could be done here too.)
	 */
	if (pei->reader != NULL)
	{
		for (i = 0; i < nworkers; i++)
			DestroyTupleQueueReader(pei->reader[i]);
		pfree(pei->reader);
		pei->reader = NULL;
	}

	/* Now wait for the workers to finish. */
	WaitForParallelWorkersToFinish(pei->pcxt);

	/*
	 * Next, accumulate buffer/WAL usage.  (This must wait for the workers to
	 * finish, or we might get incomplete data.)
	 */
	for (i = 0; i < nworkers; i++)
		InstrAccumParallelQuery(&pei->buffer_usage[i], &pei->wal_usage[i]);

	pei->finished = true;
}

/*
 * Accumulate instrumentation, and then clean up whatever ParallelExecutorInfo
 * resources still exist after ExecParallelFinish.  We separate these
 * routines because someone might want to examine the contents of the DSM
 * after ExecParallelFinish and before calling this routine.
 */
void
ExecParallelCleanup(ParallelExecutorInfo *pei)
{
	/* Accumulate instrumentation, if any. */
	if (pei->instrumentation)
		ExecParallelRetrieveInstrumentation(pei->planstate,
											pei->instrumentation);

	/* Accumulate JIT instrumentation, if any. */
	if (pei->jit_instrumentation)
		ExecParallelRetrieveJitInstrumentation(pei->planstate,
											   pei->jit_instrumentation);

	/* Free any serialized parameters. */
	if (DsaPointerIsValid(pei->param_exec))
	{
		dsa_free(pei->area, pei->param_exec);
		pei->param_exec = InvalidDsaPointer;
	}
	if (pei->area != NULL)
	{
		dsa_detach(pei->area);
		pei->area = NULL;
	}
	if (pei->pcxt != NULL)
	{
		DestroyParallelContext(pei->pcxt);
		pei->pcxt = NULL;
	}
	pfree(pei);
}

/*
 * Create a DestReceiver to write tuples we produce to the shm_mq designated
 * for that purpose.
 */
static DestReceiver *
ExecParallelGetReceiver(dsm_segment *seg, shm_toc *toc)
{
	char	   *mqspace;
	shm_mq	   *mq;

	mqspace = shm_toc_lookup(toc, PARALLEL_KEY_TUPLE_QUEUE, false);
	mqspace += ParallelWorkerNumber * PARALLEL_TUPLE_QUEUE_SIZE;
	mq = (shm_mq *) mqspace;
	shm_mq_set_sender(mq, MyProc);
	return CreateTupleQueueDestReceiver(shm_mq_attach(mq, seg, NULL));
}

/*
 * Create a QueryDesc for the PlannedStmt we are to execute, and return it.
 */
static QueryDesc *
ExecParallelGetQueryDesc(shm_toc *toc, DestReceiver *receiver,
						 int instrument_options)
{
	char	   *pstmtspace;
	char	   *paramspace;
	PlannedStmt *pstmt;
	ParamListInfo paramLI;
	char	   *queryString;

	/* Get the query string from shared memory */
	queryString = shm_toc_lookup(toc, PARALLEL_KEY_QUERY_TEXT, false);

	/* Reconstruct leader-supplied PlannedStmt. */
	pstmtspace = shm_toc_lookup(toc, PARALLEL_KEY_PLANNEDSTMT, false);
	pstmt = (PlannedStmt *) stringToNode(pstmtspace);

	/* Reconstruct ParamListInfo. */
	paramspace = shm_toc_lookup(toc, PARALLEL_KEY_PARAMLISTINFO, false);
	paramLI = RestoreParamList(&paramspace);

	/* Create a QueryDesc for the query. */
	return CreateQueryDesc(pstmt,
						   queryString,
						   GetActiveSnapshot(), InvalidSnapshot,
						   receiver, paramLI, NULL, instrument_options);
}

/*
 * Copy instrumentation information from this node and its descendants into
 * dynamic shared memory, so that the parallel leader can retrieve it.
 */
static bool
ExecParallelReportInstrumentation(PlanState *planstate,
								  SharedExecutorInstrumentation *instrumentation)
{
	int			i;
	int			plan_node_id = planstate->plan->plan_node_id;
	Instrumentation *instrument;

	InstrEndLoop(planstate->instrument);

	/*
	 * If we shuffled the plan_node_id values in ps_instrument into sorted
	 * order, we could use binary search here.  This might matter someday if
	 * we're pushing down sufficiently large plan trees.  For now, do it the
	 * slow, dumb way.
	 */
	for (i = 0; i < instrumentation->num_plan_nodes; ++i)
		if (instrumentation->plan_node_id[i] == plan_node_id)
			break;
	if (i >= instrumentation->num_plan_nodes)
		elog(ERROR, "plan node %d not found", plan_node_id);

	/*
	 * Add our statistics to the per-node, per-worker totals.  It's possible
	 * that this could happen more than once if we relaunched workers.
	 */
	instrument = GetInstrumentationArray(instrumentation);
	instrument += i * instrumentation->num_workers;
	Assert(IsParallelWorker());
	Assert(ParallelWorkerNumber < instrumentation->num_workers);
	InstrAggNode(&instrument[ParallelWorkerNumber], planstate->instrument);

	return planstate_tree_walker(planstate, ExecParallelReportInstrumentation,
								 instrumentation);
}

/*
 * Initialize the PlanState and its descendants with the information
 * retrieved from shared memory.  This has to be done once the PlanState
 * is allocated and initialized by executor; that is, after ExecutorStart().
 */
static bool
ExecParallelInitializeWorker(PlanState *planstate, ParallelWorkerContext *pwcxt)
{
	if (planstate == NULL)
		return false;

	switch (nodeTag(planstate))
	{
		case T_SeqScanState:
			if (planstate->plan->parallel_aware)
				ExecSeqScanInitializeWorker((SeqScanState *) planstate, pwcxt);
			break;
		case T_IndexScanState:
			if (planstate->plan->parallel_aware)
				ExecIndexScanInitializeWorker((IndexScanState *) planstate,
											  pwcxt);
			break;
		case T_IndexOnlyScanState:
			if (planstate->plan->parallel_aware)
				ExecIndexOnlyScanInitializeWorker((IndexOnlyScanState *) planstate,
												  pwcxt);
			break;
		case T_ForeignScanState:
			if (planstate->plan->parallel_aware)
				ExecForeignScanInitializeWorker((ForeignScanState *) planstate,
												pwcxt);
			break;
		case T_AppendState:
			if (planstate->plan->parallel_aware)
				ExecAppendInitializeWorker((AppendState *) planstate, pwcxt);
			break;
		case T_CustomScanState:
			if (planstate->plan->parallel_aware)
				ExecCustomScanInitializeWorker((CustomScanState *) planstate,
											   pwcxt);
			break;
		case T_BitmapHeapScanState:
			if (planstate->plan->parallel_aware)
				ExecBitmapHeapInitializeWorker((BitmapHeapScanState *) planstate,
											   pwcxt);
			break;
		case T_HashJoinState:
			if (planstate->plan->parallel_aware)
				ExecHashJoinInitializeWorker((HashJoinState *) planstate,
											 pwcxt);
			break;
		case T_HashState:
			/* even when not parallel-aware, for EXPLAIN ANALYZE */
			ExecHashInitializeWorker((HashState *) planstate, pwcxt);
			break;
		case T_SortState:
			/* even when not parallel-aware, for EXPLAIN ANALYZE */
			ExecSortInitializeWorker((SortState *) planstate, pwcxt);
			break;
		case T_IncrementalSortState:
			/* even when not parallel-aware, for EXPLAIN ANALYZE */
			ExecIncrementalSortInitializeWorker((IncrementalSortState *) planstate,
												pwcxt);
			break;
		case T_AggState:
			/* even when not parallel-aware, for EXPLAIN ANALYZE */
			ExecAggInitializeWorker((AggState *) planstate, pwcxt);
			break;
		case T_MemoizeState:
			/* even when not parallel-aware, for EXPLAIN ANALYZE */
			ExecMemoizeInitializeWorker((MemoizeState *) planstate, pwcxt);
			break;
		default:
			break;
	}

	return planstate_tree_walker(planstate, ExecParallelInitializeWorker,
								 pwcxt);
}

/*
 * Main entrypoint for parallel query worker processes.
 *
 * We reach this function from ParallelWorkerMain, so the setup necessary to
 * create a sensible parallel environment has already been done;
 * ParallelWorkerMain worries about stuff like the transaction state, combo
 * CID mappings, and GUC values, so we don't need to deal with any of that
 * here.
 *
 * Our job is to deal with concerns specific to the executor.  The parallel
 * group leader will have stored a serialized PlannedStmt, and it's our job
 * to execute that plan and write the resulting tuples to the appropriate
 * tuple queue.  Various bits of supporting information that we need in order
 * to do this are also stored in the dsm_segment and can be accessed through
 * the shm_toc.
 */
void
ParallelQueryMain(dsm_segment *seg, shm_toc *toc)
{
	FixedParallelExecutorState *fpes;
	BufferUsage *buffer_usage;
	WalUsage   *wal_usage;
	DestReceiver *receiver;
	QueryDesc  *queryDesc;
	SharedExecutorInstrumentation *instrumentation;
	SharedJitInstrumentation *jit_instrumentation;
	int			instrument_options = 0;
	void	   *area_space;
	dsa_area   *area;
	ParallelWorkerContext pwcxt;

	/* Get fixed-size state. */
	fpes = shm_toc_lookup(toc, PARALLEL_KEY_EXECUTOR_FIXED, false);

	/* Set up DestReceiver, SharedExecutorInstrumentation, and QueryDesc. */
	receiver = ExecParallelGetReceiver(seg, toc);
	instrumentation = shm_toc_lookup(toc, PARALLEL_KEY_INSTRUMENTATION, true);
	if (instrumentation != NULL)
		instrument_options = instrumentation->instrument_options;
	jit_instrumentation = shm_toc_lookup(toc, PARALLEL_KEY_JIT_INSTRUMENTATION,
										 true);
	queryDesc = ExecParallelGetQueryDesc(toc, receiver, instrument_options);

	/* Setting debug_query_string for individual workers */
	debug_query_string = queryDesc->sourceText;

	/* Report workers' query for monitoring purposes */
	pgstat_report_activity(STATE_RUNNING, debug_query_string);

	/* Attach to the dynamic shared memory area. */
	area_space = shm_toc_lookup(toc, PARALLEL_KEY_DSA, false);
	area = dsa_attach_in_place(area_space, seg);

	/* Start up the executor */
	queryDesc->plannedstmt->jitFlags = fpes->jit_flags;
	ExecutorStart(queryDesc, fpes->eflags);

	/* Special executor initialization steps for parallel workers */
	queryDesc->planstate->state->es_query_dsa = area;
	if (DsaPointerIsValid(fpes->param_exec))
	{
		char	   *paramexec_space;

		paramexec_space = dsa_get_address(area, fpes->param_exec);
		RestoreParamExecParams(paramexec_space, queryDesc->estate);
	}
	pwcxt.toc = toc;
	pwcxt.seg = seg;
	ExecParallelInitializeWorker(queryDesc->planstate, &pwcxt);

	/* Pass down any tuple bound */
	ExecSetTupleBound(fpes->tuples_needed, queryDesc->planstate);

	/*
	 * Prepare to track buffer/WAL usage during query execution.
	 *
	 * We do this after starting up the executor to match what happens in the
	 * leader, which also doesn't count buffer accesses and WAL activity that
	 * occur during executor startup.
	 */
	InstrStartParallelQuery();

	/*
	 * Run the plan.  If we specified a tuple bound, be careful not to demand
	 * more tuples than that.
	 */
	ExecutorRun(queryDesc,
				ForwardScanDirection,
				fpes->tuples_needed < 0 ? (int64) 0 : fpes->tuples_needed,
				true);

	/* Shut down the executor */
	ExecutorFinish(queryDesc);

	/* Report buffer/WAL usage during parallel execution. */
	buffer_usage = shm_toc_lookup(toc, PARALLEL_KEY_BUFFER_USAGE, false);
	wal_usage = shm_toc_lookup(toc, PARALLEL_KEY_WAL_USAGE, false);
	InstrEndParallelQuery(&buffer_usage[ParallelWorkerNumber],
						  &wal_usage[ParallelWorkerNumber]);

	/* Report instrumentation data if any instrumentation options are set. */
	if (instrumentation != NULL)
		ExecParallelReportInstrumentation(queryDesc->planstate,
										  instrumentation);

	/* Report JIT instrumentation data if any */
	if (queryDesc->estate->es_jit && jit_instrumentation != NULL)
	{
		Assert(ParallelWorkerNumber < jit_instrumentation->num_workers);
		jit_instrumentation->jit_instr[ParallelWorkerNumber] =
			queryDesc->estate->es_jit->instr;
	}

	/* Must do this after capturing instrumentation. */
	ExecutorEnd(queryDesc);

	/* Cleanup. */
	dsa_detach(area);
	FreeQueryDesc(queryDesc);
	receiver->rDestroy(receiver);
}
