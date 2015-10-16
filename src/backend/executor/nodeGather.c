/*-------------------------------------------------------------------------
 *
 * nodeGather.c
 *	  Support routines for scanning a plan via multiple workers.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
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
#include "utils/rel.h"


static TupleTableSlot *gather_getnext(GatherState *gatherstate);


/* ----------------------------------------------------------------
 *		ExecInitGather
 * ----------------------------------------------------------------
 */
GatherState *
ExecInitGather(Gather *node, EState *estate, int eflags)
{
	GatherState *gatherstate;

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
	ExecInitResultTupleSlot(estate, &gatherstate->ps);

	/*
	 * now initialize outer plan
	 */
	outerPlanState(gatherstate) = ExecInitNode(outerPlan(node), estate, eflags);


	gatherstate->ps.ps_TupFromTlist = false;

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&gatherstate->ps);
	ExecAssignProjectionInfo(&gatherstate->ps, NULL);

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

	slot = gather_getnext(node);

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
 * is reponsible for fetching tuples from all the queues associated
 * with worker backends used in Gather node execution and if there is
 * no data available from queues or no worker is available, it does
 * fetch the data from local node.
 */
static TupleTableSlot *
gather_getnext(GatherState *gatherstate)
{
	PlanState  *outerPlan;
	TupleTableSlot *outerTupleSlot;
	TupleTableSlot *slot;
	HeapTuple	tup;

	/*
	 * We can use projection info of Gather for the tuples received from
	 * worker backends as currently for all cases worker backends sends the
	 * projected tuple as required by Gather node.
	 */
	slot = gatherstate->ps.ps_ProjInfo->pi_slot;

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
				ExecShutdownGather(gatherstate);

			if (HeapTupleIsValid(tup))
			{
				ExecStoreTuple(tup,		/* tuple to store */
							   slot,	/* slot to store in */
							   InvalidBuffer,	/* buffer associated with this
												 * tuple */
							   true);	/* pfree this pointer if not from heap */

				return slot;
			}
		}

		if (gatherstate->need_to_scan_locally)
		{
			outerPlan = outerPlanState(gatherstate);

			outerTupleSlot = ExecProcNode(outerPlan);

			if (!TupIsNull(outerTupleSlot))
				return outerTupleSlot;

			gatherstate->need_to_scan_locally = false;
		}
	}

	return ExecClearTuple(slot);
}

/* ----------------------------------------------------------------
 *		ExecShutdownGather
 *
 *		Destroy the setup for parallel workers.  Collect all the
 *		stats after workers are stopped, else some work done by
 *		workers won't be accounted.
 * ----------------------------------------------------------------
 */
void
ExecShutdownGather(GatherState *node)
{
	/* Shut down tuple queue funnel before shutting down workers. */
	if (node->funnel != NULL)
	{
		DestroyTupleQueueFunnel(node->funnel);
		node->funnel = NULL;
	}

	/* Now shut down the workers. */
	if (node->pei != NULL)
	{
		ExecParallelFinish(node->pei);
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
	 * Re-initialize the parallel context and workers to perform rescan of
	 * relation.  We want to gracefully shutdown all the workers so that they
	 * should be able to propagate any error or other information to master
	 * backend before dying.
	 */
	ExecShutdownGather(node);

	node->initialized = false;

	ExecReScan(node->ps.lefttree);
}
