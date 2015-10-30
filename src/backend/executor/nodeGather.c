/*-------------------------------------------------------------------------
 *
 * nodeGather.c
 *	  Support routines for scanning a plan via multiple workers.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * A Gather executor launches parallel workers to run multiple copies of a
 * plan.  It can also run the plan itself, if the workers are not available
 * or have not started up yet.  It then merges all of the results it produces
 * and the results from the workers into a single output stream.  Therefore,
 * it will normally be used with a plan where running multiple copies of the
 * same plan does not produce duplicate output, such as PartialSeqScan.
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

#include "access/relscan.h"
#include "access/xact.h"
#include "executor/execdebug.h"
#include "executor/execParallel.h"
#include "executor/nodeGather.h"
#include "executor/nodeSubplan.h"
#include "executor/tqueue.h"
#include "utils/memutils.h"
#include "utils/rel.h"


static TupleTableSlot *gather_getnext(GatherState *gatherstate);
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
	bool		hasoid;
	TupleDesc	tupDesc;

	/* Gather node doesn't have innerPlan node. */
	Assert(innerPlan(node) == NULL);

	/*
	 * create state structure
	 */
	gatherstate = makeNode(GatherState);
	gatherstate->ps.plan = (Plan *) node;
	gatherstate->ps.state = estate;
	gatherstate->need_to_scan_locally = !node->single_copy;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &gatherstate->ps);

	/*
	 * initialize child expressions
	 */
	gatherstate->ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->plan.targetlist,
					 (PlanState *) gatherstate);
	gatherstate->ps.qual = (List *)
		ExecInitExpr((Expr *) node->plan.qual,
					 (PlanState *) gatherstate);

	/*
	 * tuple table initialization
	 */
	gatherstate->funnel_slot = ExecInitExtraTupleSlot(estate);
	ExecInitResultTupleSlot(estate, &gatherstate->ps);

	/*
	 * now initialize outer plan
	 */
	outerNode = outerPlan(node);
	outerPlanState(gatherstate) = ExecInitNode(outerNode, estate, eflags);

	gatherstate->ps.ps_TupFromTlist = false;

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&gatherstate->ps);
	ExecAssignProjectionInfo(&gatherstate->ps, NULL);

	/*
	 * Initialize funnel slot to same tuple descriptor as outer plan.
	 */
	if (!ExecContextForcesOids(&gatherstate->ps, &hasoid))
		hasoid = false;
	tupDesc = ExecTypeFromTL(outerNode->targetlist, hasoid);
	ExecSetSlotDescriptor(gatherstate->funnel_slot, tupDesc);

	return gatherstate;
}

/* ----------------------------------------------------------------
 *		ExecGather(node)
 *
 *		Scans the relation via multiple workers and returns
 *		the next qualifying tuple.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecGather(GatherState *node)
{
	int			i;
	TupleTableSlot *slot;
	TupleTableSlot *resultSlot;
	ExprDoneCond isDone;
	ExprContext *econtext;

	/*
	 * Initialize the parallel context and workers on first execution. We do
	 * this on first execution rather than during node initialization, as it
	 * needs to allocate large dynamic segement, so it is better to do if it
	 * is really needed.
	 */
	if (!node->initialized)
	{
		EState	   *estate = node->ps.state;
		Gather	   *gather = (Gather *) node->ps.plan;

		/*
		 * Sometimes we might have to run without parallelism; but if
		 * parallel mode is active then we can try to fire up some workers.
		 */
		if (gather->num_workers > 0 && IsInParallelMode())
		{
			bool	got_any_worker = false;

			/* Initialize the workers required to execute Gather node. */
			if (!node->pei)
				node->pei = ExecInitParallelPlan(node->ps.lefttree,
												 estate,
												 gather->num_workers);

			/*
			 * Register backend workers. We might not get as many as we
			 * requested, or indeed any at all.
			 */
			LaunchParallelWorkers(node->pei->pcxt);

			/* Set up a tuple queue to collect the results. */
			node->funnel = CreateTupleQueueFunnel();
			for (i = 0; i < node->pei->pcxt->nworkers; ++i)
			{
				if (node->pei->pcxt->worker[i].bgwhandle)
				{
					shm_mq_set_handle(node->pei->tqueue[i],
									  node->pei->pcxt->worker[i].bgwhandle);
					RegisterTupleQueueOnFunnel(node->funnel,
											   node->pei->tqueue[i]);
					got_any_worker = true;
				}
			}

			/* No workers?  Then never mind. */
			if (!got_any_worker)
				ExecShutdownGather(node);
		}

		/* Run plan locally if no workers or not single-copy. */
		node->need_to_scan_locally = (node->funnel == NULL)
			|| !gather->single_copy;
		node->initialized = true;
	}

	/*
	 * Check to see if we're still projecting out tuples from a previous scan
	 * tuple (because there is a function-returning-set in the projection
	 * expressions).  If so, try to project another one.
	 */
	if (node->ps.ps_TupFromTlist)
	{
		resultSlot = ExecProject(node->ps.ps_ProjInfo, &isDone);
		if (isDone == ExprMultipleResult)
			return resultSlot;
		/* Done with that source tuple... */
		node->ps.ps_TupFromTlist = false;
	}

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.  Note we can't do this
	 * until we're done projecting.
	 */
	econtext = node->ps.ps_ExprContext;
	ResetExprContext(econtext);

	/* Get and return the next tuple, projecting if necessary. */
	for (;;)
	{
		/*
		 * Get next tuple, either from one of our workers, or by running the
		 * plan ourselves.
		 */
		slot = gather_getnext(node);
		if (TupIsNull(slot))
			return NULL;

		/*
		 * form the result tuple using ExecProject(), and return it --- unless
		 * the projection produces an empty set, in which case we must loop
		 * back around for another tuple
		 */
		econtext->ecxt_outertuple = slot;
		resultSlot = ExecProject(node->ps.ps_ProjInfo, &isDone);

		if (isDone != ExprEndResult)
		{
			node->ps.ps_TupFromTlist = (isDone == ExprMultipleResult);
			return resultSlot;
		}
	}

	return slot;
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
	ExecShutdownGather(node);
	ExecFreeExprContext(&node->ps);
	ExecClearTuple(node->ps.ps_ResultTupleSlot);
	ExecEndNode(outerPlanState(node));
}

/*
 * gather_getnext
 *
 * Get the next tuple from shared memory queue.  This function
 * is responsible for fetching tuples from all the queues associated
 * with worker backends used in Gather node execution and if there is
 * no data available from queues or no worker is available, it does
 * fetch the data from local node.
 */
static TupleTableSlot *
gather_getnext(GatherState *gatherstate)
{
	PlanState  *outerPlan = outerPlanState(gatherstate);
	TupleTableSlot *outerTupleSlot;
	TupleTableSlot *fslot = gatherstate->funnel_slot;
	HeapTuple	tup;

	while (gatherstate->funnel != NULL || gatherstate->need_to_scan_locally)
	{
		if (gatherstate->funnel != NULL)
		{
			bool		done = false;

			/* wait only if local scan is done */
			tup = TupleQueueFunnelNext(gatherstate->funnel,
									   gatherstate->need_to_scan_locally,
									   &done);
			if (done)
				ExecShutdownGatherWorkers(gatherstate);

			if (HeapTupleIsValid(tup))
			{
				ExecStoreTuple(tup,		/* tuple to store */
							   fslot,	/* slot in which to store the tuple */
							   InvalidBuffer,	/* buffer associated with this
												 * tuple */
							   true);	/* pfree this pointer if not from heap */

				return fslot;
			}
		}

		if (gatherstate->need_to_scan_locally)
		{
			outerTupleSlot = ExecProcNode(outerPlan);

			if (!TupIsNull(outerTupleSlot))
				return outerTupleSlot;

			gatherstate->need_to_scan_locally = false;
		}
	}

	return ExecClearTuple(fslot);
}

/* ----------------------------------------------------------------
 *		ExecShutdownGatherWorkers
 *
 *		Destroy the parallel workers.  Collect all the stats after
 *		workers are stopped, else some work done by workers won't be
 *		accounted.
 * ----------------------------------------------------------------
 */
void
ExecShutdownGatherWorkers(GatherState *node)
{
	/* Shut down tuple queue funnel before shutting down workers. */
	if (node->funnel != NULL)
	{
		DestroyTupleQueueFunnel(node->funnel);
		node->funnel = NULL;
	}

	/* Now shut down the workers. */
	if (node->pei != NULL)
		ExecParallelFinish(node->pei);
}

/* ----------------------------------------------------------------
 *		ExecShutdownGather
 *
 *		Destroy the setup for parallel workers including parallel context.
 *		Collect all the stats after workers are stopped, else some work
 *		done by workers won't be accounted.
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
 *		Re-initialize the workers and rescans a relation via them.
 * ----------------------------------------------------------------
 */
void
ExecReScanGather(GatherState *node)
{
	/*
	 * Re-initialize the parallel workers to perform rescan of relation.
	 * We want to gracefully shutdown all the workers so that they
	 * should be able to propagate any error or other information to master
	 * backend before dying.  Parallel context will be reused for rescan.
	 */
	ExecShutdownGatherWorkers(node);

	node->initialized = false;

	if (node->pei)
	{
		ReinitializeParallelDSM(node->pei->pcxt);
		node->pei->tqueue =
				ExecParallelReinitializeTupleQueues(node->pei->pcxt);
	}

	ExecReScan(node->ps.lefttree);
}
