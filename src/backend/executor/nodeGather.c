/*-------------------------------------------------------------------------
 *
 * nodeGather.c
 *	  Support routines for scanning a plan via multiple workers.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * A Gather executor launches parallel workers to run multiple copies of a
 * plan.  It can also run the plan itself, if the workers are not available
 * or have not started up yet.  It then merges all of the results it produces
 * and the results from the workers into a single output stream.  Therefore,
 * it will normally be used with a plan where running multiple copies of the
 * same plan does not produce duplicate output, such as parallel-aware
 * SeqScan.
 *
 * Alternatively, a Gather node can be configured to use just one worker
 * and the single-copy flag can be set.  In this case, the Gather node will
 * run the plan in one worker and will not execute the plan itself.  In
 * this case, it simply returns whatever tuples were returned by the worker.
 * If a worker cannot be obtained, then it will run the plan itself and
 * return the results.  Therefore, a plan used with a single-copy Gather
 * node need not be parallel-aware.
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeGather.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/execParallel.h"
#include "executor/executor.h"
#include "executor/nodeGather.h"
#include "executor/tqueue.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "utils/wait_event.h"


static TupleTableSlot *ExecGather(PlanState *pstate);
static TupleTableSlot *gather_getnext(GatherState *gatherstate);
static MinimalTuple gather_readnext(GatherState *gatherstate);
static void ExecShutdownGatherWorkers(GatherState *node);


/* ----------------------------------------------------------------
 *		ExecInitGather
 * ----------------------------------------------------------------
 */
GatherState *
ExecInitGather(Gather *node, EState *estate, int eflags)
{
	GatherState *gatherstate;
	Plan	   *outerNode;
	TupleDesc	tupDesc;

	/* Gather node doesn't have innerPlan node. */
	Assert(innerPlan(node) == NULL);

	/*
	 * create state structure
	 */
	gatherstate = makeNode(GatherState);
	gatherstate->ps.plan = (Plan *) node;
	gatherstate->ps.state = estate;
	gatherstate->ps.ExecProcNode = ExecGather;

	gatherstate->initialized = false;
	gatherstate->need_to_scan_locally =
		!node->single_copy && parallel_leader_participation;
	gatherstate->tuples_needed = -1;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &gatherstate->ps);

	/*
	 * now initialize outer plan
	 */
	outerNode = outerPlan(node);
	outerPlanState(gatherstate) = ExecInitNode(outerNode, estate, eflags);
	tupDesc = ExecGetResultType(outerPlanState(gatherstate));

	/*
	 * Leader may access ExecProcNode result directly (if
	 * need_to_scan_locally), or from workers via tuple queue.  So we can't
	 * trivially rely on the slot type being fixed for expressions evaluated
	 * within this node.
	 */
	gatherstate->ps.outeropsset = true;
	gatherstate->ps.outeropsfixed = false;

	/*
	 * Initialize result type and projection.
	 */
	ExecInitResultTypeTL(&gatherstate->ps);
	ExecConditionalAssignProjectionInfo(&gatherstate->ps, tupDesc, OUTER_VAR);

	/*
	 * Without projections result slot type is not trivially known, see
	 * comment above.
	 */
	if (gatherstate->ps.ps_ProjInfo == NULL)
	{
		gatherstate->ps.resultopsset = true;
		gatherstate->ps.resultopsfixed = false;
	}

	/*
	 * Initialize funnel slot to same tuple descriptor as outer plan.
	 */
	gatherstate->funnel_slot = ExecInitExtraTupleSlot(estate, tupDesc,
													  &TTSOpsMinimalTuple);

	/*
	 * Gather doesn't support checking a qual (it's always more efficient to
	 * do it in the child node).
	 */
	Assert(!node->plan.qual);

	return gatherstate;
}

/* ----------------------------------------------------------------
 *		ExecGather(node)
 *
 *		Scans the relation via multiple workers and returns
 *		the next qualifying tuple.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecGather(PlanState *pstate)
{
	GatherState *node = castNode(GatherState, pstate);
	TupleTableSlot *slot;
	ExprContext *econtext;

	CHECK_FOR_INTERRUPTS();

	/*
	 * Initialize the parallel context and workers on first execution. We do
	 * this on first execution rather than during node initialization, as it
	 * needs to allocate a large dynamic segment, so it is better to do it
	 * only if it is really needed.
	 */
	if (!node->initialized)
	{
		EState	   *estate = node->ps.state;
		Gather	   *gather = (Gather *) node->ps.plan;

		/*
		 * Sometimes we might have to run without parallelism; but if parallel
		 * mode is active then we can try to fire up some workers.
		 */
		if (gather->num_workers > 0 && estate->es_use_parallel_mode)
		{
			ParallelContext *pcxt;

			/* Initialize, or re-initialize, shared state needed by workers. */
			if (!node->pei)
				node->pei = ExecInitParallelPlan(outerPlanState(node),
												 estate,
												 gather->initParam,
												 gather->num_workers,
												 node->tuples_needed);
			else
				ExecParallelReinitialize(outerPlanState(node),
										 node->pei,
										 gather->initParam);

			/*
			 * Register backend workers. We might not get as many as we
			 * requested, or indeed any at all.
			 */
			pcxt = node->pei->pcxt;
			LaunchParallelWorkers(pcxt);
			/* We save # workers launched for the benefit of EXPLAIN */
			node->nworkers_launched = pcxt->nworkers_launched;

			/*
			 * Count number of workers originally wanted and actually
			 * launched.
			 */
			estate->es_parallel_workers_to_launch += pcxt->nworkers_to_launch;
			estate->es_parallel_workers_launched += pcxt->nworkers_launched;

			/* Set up tuple queue readers to read the results. */
			if (pcxt->nworkers_launched > 0)
			{
				ExecParallelCreateReaders(node->pei);
				/* Make a working array showing the active readers */
				node->nreaders = pcxt->nworkers_launched;
				node->reader = (TupleQueueReader **)
					palloc(node->nreaders * sizeof(TupleQueueReader *));
				memcpy(node->reader, node->pei->reader,
					   node->nreaders * sizeof(TupleQueueReader *));
			}
			else
			{
				/* No workers?	Then never mind. */
				node->nreaders = 0;
				node->reader = NULL;
			}
			node->nextreader = 0;
		}

		/* Run plan locally if no workers or enabled and not single-copy. */
		node->need_to_scan_locally = (node->nreaders == 0)
			|| (!gather->single_copy && parallel_leader_participation);
		node->initialized = true;
	}

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.
	 */
	econtext = node->ps.ps_ExprContext;
	ResetExprContext(econtext);

	/*
	 * Get next tuple, either from one of our workers, or by running the plan
	 * ourselves.
	 */
	slot = gather_getnext(node);
	if (TupIsNull(slot))
		return NULL;

	/* If no projection is required, we're done. */
	if (node->ps.ps_ProjInfo == NULL)
		return slot;

	/*
	 * Form the result tuple using ExecProject(), and return it.
	 */
	econtext->ecxt_outertuple = slot;
	return ExecProject(node->ps.ps_ProjInfo);
}

/* ----------------------------------------------------------------
 *		ExecEndGather
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndGather(GatherState *node)
{
	ExecEndNode(outerPlanState(node));	/* let children clean up first */
	ExecShutdownGather(node);
}

/*
 * Read the next tuple.  We might fetch a tuple from one of the tuple queues
 * using gather_readnext, or if no tuple queue contains a tuple and the
 * single_copy flag is not set, we might generate one locally instead.
 */
static TupleTableSlot *
gather_getnext(GatherState *gatherstate)
{
	PlanState  *outerPlan = outerPlanState(gatherstate);
	TupleTableSlot *outerTupleSlot;
	TupleTableSlot *fslot = gatherstate->funnel_slot;
	MinimalTuple tup;

	while (gatherstate->nreaders > 0 || gatherstate->need_to_scan_locally)
	{
		CHECK_FOR_INTERRUPTS();

		if (gatherstate->nreaders > 0)
		{
			tup = gather_readnext(gatherstate);

			if (HeapTupleIsValid(tup))
			{
				ExecStoreMinimalTuple(tup,	/* tuple to store */
									  fslot,	/* slot to store the tuple */
									  false);	/* don't pfree tuple  */
				return fslot;
			}
		}

		if (gatherstate->need_to_scan_locally)
		{
			EState	   *estate = gatherstate->ps.state;

			/* Install our DSA area while executing the plan. */
			estate->es_query_dsa =
				gatherstate->pei ? gatherstate->pei->area : NULL;
			outerTupleSlot = ExecProcNode(outerPlan);
			estate->es_query_dsa = NULL;

			if (!TupIsNull(outerTupleSlot))
				return outerTupleSlot;

			gatherstate->need_to_scan_locally = false;
		}
	}

	return ExecClearTuple(fslot);
}

/*
 * Attempt to read a tuple from one of our parallel workers.
 */
static MinimalTuple
gather_readnext(GatherState *gatherstate)
{
	int			nvisited = 0;

	for (;;)
	{
		TupleQueueReader *reader;
		MinimalTuple tup;
		bool		readerdone;

		/* Check for async events, particularly messages from workers. */
		CHECK_FOR_INTERRUPTS();

		/*
		 * Attempt to read a tuple, but don't block if none is available.
		 *
		 * Note that TupleQueueReaderNext will just return NULL for a worker
		 * which fails to initialize.  We'll treat that worker as having
		 * produced no tuples; WaitForParallelWorkersToFinish will error out
		 * when we get there.
		 */
		Assert(gatherstate->nextreader < gatherstate->nreaders);
		reader = gatherstate->reader[gatherstate->nextreader];
		tup = TupleQueueReaderNext(reader, true, &readerdone);

		/*
		 * If this reader is done, remove it from our working array of active
		 * readers.  If all readers are done, we're outta here.
		 */
		if (readerdone)
		{
			Assert(!tup);
			--gatherstate->nreaders;
			if (gatherstate->nreaders == 0)
			{
				ExecShutdownGatherWorkers(gatherstate);
				return NULL;
			}
			memmove(&gatherstate->reader[gatherstate->nextreader],
					&gatherstate->reader[gatherstate->nextreader + 1],
					sizeof(TupleQueueReader *)
					* (gatherstate->nreaders - gatherstate->nextreader));
			if (gatherstate->nextreader >= gatherstate->nreaders)
				gatherstate->nextreader = 0;
			continue;
		}

		/* If we got a tuple, return it. */
		if (tup)
			return tup;

		/*
		 * Advance nextreader pointer in round-robin fashion.  Note that we
		 * only reach this code if we weren't able to get a tuple from the
		 * current worker.  We used to advance the nextreader pointer after
		 * every tuple, but it turns out to be much more efficient to keep
		 * reading from the same queue until that would require blocking.
		 */
		gatherstate->nextreader++;
		if (gatherstate->nextreader >= gatherstate->nreaders)
			gatherstate->nextreader = 0;

		/* Have we visited every (surviving) TupleQueueReader? */
		nvisited++;
		if (nvisited >= gatherstate->nreaders)
		{
			/*
			 * If (still) running plan locally, return NULL so caller can
			 * generate another tuple from the local copy of the plan.
			 */
			if (gatherstate->need_to_scan_locally)
				return NULL;

			/* Nothing to do except wait for developments. */
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
							 WAIT_EVENT_EXECUTE_GATHER);
			ResetLatch(MyLatch);
			nvisited = 0;
		}
	}
}

/* ----------------------------------------------------------------
 *		ExecShutdownGatherWorkers
 *
 *		Stop all the parallel workers.
 * ----------------------------------------------------------------
 */
static void
ExecShutdownGatherWorkers(GatherState *node)
{
	if (node->pei != NULL)
		ExecParallelFinish(node->pei);

	/* Flush local copy of reader array */
	if (node->reader)
		pfree(node->reader);
	node->reader = NULL;
}

/* ----------------------------------------------------------------
 *		ExecShutdownGather
 *
 *		Destroy the setup for parallel workers including parallel context.
 * ----------------------------------------------------------------
 */
void
ExecShutdownGather(GatherState *node)
{
	ExecShutdownGatherWorkers(node);

	/* Now destroy the parallel context. */
	if (node->pei != NULL)
	{
		ExecParallelCleanup(node->pei);
		node->pei = NULL;
	}
}

/* ----------------------------------------------------------------
 *						Join Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecReScanGather
 *
 *		Prepare to re-scan the result of a Gather.
 * ----------------------------------------------------------------
 */
void
ExecReScanGather(GatherState *node)
{
	Gather	   *gather = (Gather *) node->ps.plan;
	PlanState  *outerPlan = outerPlanState(node);

	/* Make sure any existing workers are gracefully shut down */
	ExecShutdownGatherWorkers(node);

	/* Mark node so that shared state will be rebuilt at next call */
	node->initialized = false;

	/*
	 * Set child node's chgParam to tell it that the next scan might deliver a
	 * different set of rows within the leader process.  (The overall rowset
	 * shouldn't change, but the leader process's subset might; hence nodes
	 * between here and the parallel table scan node mustn't optimize on the
	 * assumption of an unchanging rowset.)
	 */
	if (gather->rescan_param >= 0)
		outerPlan->chgParam = bms_add_member(outerPlan->chgParam,
											 gather->rescan_param);

	/*
	 * If chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.  Note: because this does nothing if we have a
	 * rescan_param, it's currently guaranteed that parallel-aware child nodes
	 * will not see a ReScan call until after they get a ReInitializeDSM call.
	 * That ordering might not be something to rely on, though.  A good rule
	 * of thumb is that ReInitializeDSM should reset only shared state, ReScan
	 * should reset only local state, and anything that depends on both of
	 * those steps being finished must wait until the first ExecProcNode call.
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}
