/*-------------------------------------------------------------------------
 *
 * nodeAppend.c
 *	  routines to handle append nodes.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeAppend.c,v 1.71 2006/10/04 00:29:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/* INTERFACE ROUTINES
 *		ExecInitAppend	- initialize the append node
 *		ExecAppend		- retrieve the next tuple from the node
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
 *				select name from person
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

#include "executor/execdebug.h"
#include "executor/nodeAppend.h"

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
		 * if scanning in reverse, we start at the last scan in the list and
		 * then proceed back to the first.. in any case we inform ExecAppend
		 * that we are at the end of the line by returning FALSE
		 */
		appendstate->as_whichplan = appendstate->as_firstplan;
		return FALSE;
	}
	else if (whichplan > appendstate->as_lastplan)
	{
		/*
		 * as above, end the scan if we go beyond the last scan in our list..
		 */
		appendstate->as_whichplan = appendstate->as_lastplan;
		return FALSE;
	}
	else
	{
		/*
		 * initialize the scan
		 *
		 * If we are controlling the target relation, select the proper active
		 * ResultRelInfo and junk filter for this target.
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
 *		block instead of that of the call to ExecAppend.)
 *
 *		Special case: during an EvalPlanQual recheck query of an inherited
 *		target relation, we only want to initialize and scan the single
 *		subplan that corresponds to the target relation being checked.
 * ----------------------------------------------------------------
 */
AppendState *
ExecInitAppend(Append *node, EState *estate, int eflags)
{
	AppendState *appendstate = makeNode(AppendState);
	PlanState **appendplanstates;
	int			nplans;
	int			i;
	Plan	   *initNode;

	/* check for unsupported flags */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * Set up empty vector of subplan states
	 */
	nplans = list_length(node->appendplans);

	appendplanstates = (PlanState **) palloc0(nplans * sizeof(PlanState *));

	/*
	 * create new AppendState for our append node
	 */
	appendstate->ps.plan = (Plan *) node;
	appendstate->ps.state = estate;
	appendstate->appendplans = appendplanstates;
	appendstate->as_nplans = nplans;

	/*
	 * Do we want to scan just one subplan?  (Special case for EvalPlanQual)
	 * XXX pretty dirty way of determining that this case applies ...
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
	 * append nodes still have Result slots, which hold pointers to tuples, so
	 * we have to initialize them.
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

		initNode = (Plan *) list_nth(node->appendplans, i);
		appendplanstates[i] = ExecInitNode(initNode, estate, eflags);
	}

	/*
	 * Initialize tuple type.  (Note: in an inherited UPDATE situation, the
	 * tuple type computed here corresponds to the parent table, which is
	 * really a lie since tuples returned from child subplans will not all
	 * look the same.)
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
	ListCell   *plan;
	int			nSlots = 0;

	foreach(plan, node->appendplans)
		nSlots += ExecCountSlotsNode((Plan *) lfirst(plan));
	return nSlots + APPEND_NSLOTS;
}

/* ----------------------------------------------------------------
 *	   ExecAppend
 *
 *		Handles iteration over multiple subplans.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecAppend(AppendState *node)
{
	for (;;)
	{
		PlanState  *subnode;
		TupleTableSlot *result;

		/*
		 * figure out which subplan we are currently processing
		 */
		subnode = node->appendplans[node->as_whichplan];

		/*
		 * get a tuple from the subplan
		 */
		result = ExecProcNode(subnode);

		if (!TupIsNull(result))
		{
			/*
			 * If the subplan gave us something then return it as-is. We do
			 * NOT make use of the result slot that was set up in
			 * ExecInitAppend, first because there's no reason to and second
			 * because it may have the wrong tuple descriptor in
			 * inherited-UPDATE cases.
			 */
			return result;
		}

		/*
		 * Go on to the "next" subplan in the appropriate direction. If no
		 * more subplans, return the empty slot set up for us by
		 * ExecInitAppend.
		 */
		if (ScanDirectionIsForward(node->ps.state->es_direction))
			node->as_whichplan++;
		else
			node->as_whichplan--;
		if (!exec_append_initialize_next(node))
			return ExecClearTuple(node->ps.ps_ResultTupleSlot);

		/* Else loop back and try to get a tuple from the new subplan */
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
		 * If chgParam of subnode is not null then plan will be re-scanned by
		 * first ExecProcNode.	However, if caller is passing us an exprCtxt
		 * then forcibly rescan all the subnodes now, so that we can pass the
		 * exprCtxt down to the subnodes (needed for appendrel indexscan).
		 */
		if (subnode->chgParam == NULL || exprCtxt != NULL)
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
