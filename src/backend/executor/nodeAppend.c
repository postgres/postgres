/*-------------------------------------------------------------------------
 *
 * nodeAppend.c
 *	  routines to handle append nodes.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeAppend.c,v 1.54.4.1 2004/01/22 02:23:35 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/* INTERFACE ROUTINES
 *		ExecInitAppend	- initialize the append node
 *		ExecProcAppend	- retrieve the next tuple from the node
 *		ExecEndAppend	- shut down the append node
 *		ExecReScanAppend - rescan the append node
 *
 *	 NOTES
 *		Each append node contains a list of one or more subplans which
 *		must be iteratively processed (forwards or backwards).
 *		Tuples are retrieved by executing the 'whichplan'th subplan
 *		until the subplan stops returning tuples, at which point that
 *		plan is shut down and the next started up.
 *
 *		Append nodes don't make use of their left and right
 *		subtrees, rather they maintain a list of subplans so
 *		a typical append node looks like this in the plan tree:
 *
 *				   ...
 *				   /
 *				Append -------+------+------+--- nil
 *				/	\		  |		 |		|
 *			  nil	nil		 ...	...    ...
 *								 subplans
 *
 *		Append nodes are currently used for unions, and to support
 *		inheritance queries, where several relations need to be scanned.
 *		For example, in our standard person/student/employee/student-emp
 *		example, where student and employee inherit from person
 *		and student-emp inherits from student and employee, the
 *		query:
 *
 *				retrieve (e.name) from e in person*
 *
 *		generates the plan:
 *
 *				  |
 *				Append -------+-------+--------+--------+
 *				/	\		  |		  |		   |		|
 *			  nil	nil		 Scan	 Scan	  Scan	   Scan
 *							  |		  |		   |		|
 *							person employee student student-emp
 */

#include "postgres.h"

#include "access/heapam.h"
#include "executor/execdebug.h"
#include "executor/nodeAppend.h"
#include "parser/parsetree.h"

static bool exec_append_initialize_next(AppendState *appendstate);


/* ----------------------------------------------------------------
 *		exec_append_initialize_next
 *
 *		Sets up the append state node for the "next" scan.
 *
 *		Returns t iff there is a "next" scan to process.
 * ----------------------------------------------------------------
 */
static bool
exec_append_initialize_next(AppendState *appendstate)
{
	EState	   *estate;
	int			whichplan;

	/*
	 * get information from the append node
	 */
	estate = appendstate->ps.state;
	whichplan = appendstate->as_whichplan;

	if (whichplan < appendstate->as_firstplan)
	{
		/*
		 * if scanning in reverse, we start at the last scan in the list
		 * and then proceed back to the first.. in any case we inform
		 * ExecProcAppend that we are at the end of the line by returning
		 * FALSE
		 */
		appendstate->as_whichplan = appendstate->as_firstplan;
		return FALSE;
	}
	else if (whichplan > appendstate->as_lastplan)
	{
		/*
		 * as above, end the scan if we go beyond the last scan in our
		 * list..
		 */
		appendstate->as_whichplan = appendstate->as_lastplan;
		return FALSE;
	}
	else
	{
		/*
		 * initialize the scan
		 *
		 * If we are controlling the target relation, select the proper
		 * active ResultRelInfo and junk filter for this target.
		 */
		if (((Append *) appendstate->ps.plan)->isTarget)
		{
			Assert(whichplan < estate->es_num_result_relations);
			estate->es_result_relation_info =
				estate->es_result_relations + whichplan;
			estate->es_junkFilter =
				estate->es_result_relation_info->ri_junkFilter;
		}

		return TRUE;
	}
}

/* ----------------------------------------------------------------
 *		ExecInitAppend
 *
 *		Begin all of the subscans of the append node.
 *
 *	   (This is potentially wasteful, since the entire result of the
 *		append node may not be scanned, but this way all of the
 *		structures get allocated in the executor's top level memory
 *		block instead of that of the call to ExecProcAppend.)
 *
 *		Special case: during an EvalPlanQual recheck query of an inherited
 *		target relation, we only want to initialize and scan the single
 *		subplan that corresponds to the target relation being checked.
 * ----------------------------------------------------------------
 */
AppendState *
ExecInitAppend(Append *node, EState *estate)
{
	AppendState *appendstate = makeNode(AppendState);
	PlanState **appendplanstates;
	int			nplans;
	int			i;
	Plan	   *initNode;

	CXT1_printf("ExecInitAppend: context is %d\n", CurrentMemoryContext);

	/*
	 * Set up empty vector of subplan states
	 */
	nplans = length(node->appendplans);

	appendplanstates = (PlanState **) palloc0(nplans * sizeof(PlanState *));

	/*
	 * create new AppendState for our append node
	 */
	appendstate->ps.plan = (Plan *) node;
	appendstate->ps.state = estate;
	appendstate->appendplans = appendplanstates;
	appendstate->as_nplans = nplans;

	/*
	 * Do we want to scan just one subplan?  (Special case for
	 * EvalPlanQual) XXX pretty dirty way of determining that this case
	 * applies ...
	 */
	if (node->isTarget && estate->es_evTuple != NULL)
	{
		int			tplan;

		tplan = estate->es_result_relation_info - estate->es_result_relations;
		Assert(tplan >= 0 && tplan < nplans);

		appendstate->as_firstplan = tplan;
		appendstate->as_lastplan = tplan;
	}
	else
	{
		/* normal case, scan all subplans */
		appendstate->as_firstplan = 0;
		appendstate->as_lastplan = nplans - 1;
	}

	/*
	 * Miscellaneous initialization
	 *
	 * Append plans don't have expression contexts because they never call
	 * ExecQual or ExecProject.
	 */

#define APPEND_NSLOTS 1

	/*
	 * append nodes still have Result slots, which hold pointers to
	 * tuples, so we have to initialize them.
	 */
	ExecInitResultTupleSlot(estate, &appendstate->ps);

	/*
	 * call ExecInitNode on each of the plans to be executed and save the
	 * results into the array "appendplans".  Note we *must* set
	 * estate->es_result_relation_info correctly while we initialize each
	 * sub-plan; ExecContextForcesOids depends on that!
	 */
	for (i = appendstate->as_firstplan; i <= appendstate->as_lastplan; i++)
	{
		appendstate->as_whichplan = i;
		exec_append_initialize_next(appendstate);

		initNode = (Plan *) nth(i, node->appendplans);
		appendplanstates[i] = ExecInitNode(initNode, estate);
	}

	/*
	 * initialize tuple type
	 */
	ExecAssignResultTypeFromTL(&appendstate->ps);
	appendstate->ps.ps_ProjInfo = NULL;

	/*
	 * return the result from the first subplan's initialization
	 */
	appendstate->as_whichplan = appendstate->as_firstplan;
	exec_append_initialize_next(appendstate);

	return appendstate;
}

int
ExecCountSlotsAppend(Append *node)
{
	List	   *plan;
	int			nSlots = 0;

	foreach(plan, node->appendplans)
		nSlots += ExecCountSlotsNode((Plan *) lfirst(plan));
	return nSlots + APPEND_NSLOTS;
}

/* ----------------------------------------------------------------
 *	   ExecProcAppend
 *
 *		Handles the iteration over the multiple scans.
 *
 *	   NOTE: Can't call this ExecAppend, that name is used in execMain.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecProcAppend(AppendState *node)
{
	EState	   *estate;
	int			whichplan;
	PlanState  *subnode;
	TupleTableSlot *result;
	TupleTableSlot *result_slot;
	ScanDirection direction;

	/*
	 * get information from the node
	 */
	estate = node->ps.state;
	direction = estate->es_direction;
	whichplan = node->as_whichplan;
	result_slot = node->ps.ps_ResultTupleSlot;

	/*
	 * figure out which subplan we are currently processing
	 */
	subnode = node->appendplans[whichplan];

	/*
	 * get a tuple from the subplan
	 */
	result = ExecProcNode(subnode);

	if (!TupIsNull(result))
	{
		/*
		 * if the subplan gave us something then place a copy of whatever
		 * we get into our result slot and return it.
		 *
		 * Note we rely on the subplan to retain ownership of the tuple for
		 * as long as we need it --- we don't copy it.
		 */
		return ExecStoreTuple(result->val, result_slot, InvalidBuffer, false);
	}
	else
	{
		/*
		 * .. go on to the "next" subplan in the appropriate direction and
		 * try processing again (recursively)
		 */
		if (ScanDirectionIsForward(direction))
			node->as_whichplan++;
		else
			node->as_whichplan--;

		/*
		 * return something from next node or an empty slot if all of our
		 * subplans have been exhausted.
		 */
		if (exec_append_initialize_next(node))
		{
			ExecSetSlotDescriptorIsNew(result_slot, true);
			return ExecProcAppend(node);
		}
		else
			return ExecClearTuple(result_slot);
	}
}

/* ----------------------------------------------------------------
 *		ExecEndAppend
 *
 *		Shuts down the subscans of the append node.
 *
 *		Returns nothing of interest.
 * ----------------------------------------------------------------
 */
void
ExecEndAppend(AppendState *node)
{
	PlanState **appendplans;
	int			nplans;
	int			i;

	/*
	 * get information from the node
	 */
	appendplans = node->appendplans;
	nplans = node->as_nplans;

	/*
	 * shut down each of the subscans (that we've initialized)
	 */
	for (i = 0; i < nplans; i++)
	{
		if (appendplans[i])
			ExecEndNode(appendplans[i]);
	}
}

void
ExecReScanAppend(AppendState *node, ExprContext *exprCtxt)
{
	int			i;

	for (i = node->as_firstplan; i <= node->as_lastplan; i++)
	{
		PlanState  *subnode = node->appendplans[i];

		/*
		 * ExecReScan doesn't know about my subplans, so I have to do
		 * changed-parameter signaling myself.
		 */
		if (node->ps.chgParam != NULL)
			UpdateChangedParamSet(subnode, node->ps.chgParam);

		/*
		 * if chgParam of subnode is not null then plan will be re-scanned
		 * by first ExecProcNode.
		 */
		if (subnode->chgParam == NULL)
		{
			/* make sure estate is correct for this subnode (needed??) */
			node->as_whichplan = i;
			exec_append_initialize_next(node);
			ExecReScan(subnode, exprCtxt);
		}
	}
	node->as_whichplan = node->as_firstplan;
	exec_append_initialize_next(node);
}
