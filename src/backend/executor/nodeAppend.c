/*-------------------------------------------------------------------------
 *
 * nodeAppend.c
 *	  routines to handle append nodes.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeAppend.c,v 1.40 2001/03/22 06:16:12 momjian Exp $
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

static bool exec_append_initialize_next(Append *node);

/* ----------------------------------------------------------------
 *		exec_append_initialize_next
 *
 *		Sets up the append node state (i.e. the append state node)
 *		for the "next" scan.
 *
 *		Returns t iff there is a "next" scan to process.
 * ----------------------------------------------------------------
 */
static bool
exec_append_initialize_next(Append *node)
{
	EState	   *estate;
	AppendState *appendstate;
	int			whichplan;
	int			nplans;

	/*
	 * get information from the append node
	 */
	estate = node->plan.state;
	appendstate = node->appendstate;
	whichplan = appendstate->as_whichplan;
	nplans = appendstate->as_nplans;

	if (whichplan < 0)
	{

		/*
		 * if scanning in reverse, we start at the last scan in the list
		 * and then proceed back to the first.. in any case we inform
		 * ExecProcAppend that we are at the end of the line by returning
		 * FALSE
		 */
		appendstate->as_whichplan = 0;
		return FALSE;
	}
	else if (whichplan >= nplans)
	{

		/*
		 * as above, end the scan if we go beyond the last scan in our
		 * list..
		 */
		appendstate->as_whichplan = nplans - 1;
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
		if (node->isTarget)
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
 *		Begins all of the subscans of the append node, storing the
 *		scan structures in the 'initialized' vector of the append-state
 *		structure.
 *
 *	   (This is potentially wasteful, since the entire result of the
 *		append node may not be scanned, but this way all of the
 *		structures get allocated in the executor's top level memory
 *		block instead of that of the call to ExecProcAppend.)
 *
 *		Returns the scan result of the first scan.
 * ----------------------------------------------------------------
 */
bool
ExecInitAppend(Append *node, EState *estate, Plan *parent)
{
	AppendState *appendstate;
	int			nplans;
	List	   *appendplans;
	bool	   *initialized;
	int			i;
	Plan	   *initNode;

	CXT1_printf("ExecInitAppend: context is %d\n", CurrentMemoryContext);

	/*
	 * assign execution state to node and get information for append state
	 */
	node->plan.state = estate;

	appendplans = node->appendplans;
	nplans = length(appendplans);

	initialized = (bool *) palloc(nplans * sizeof(bool));
	MemSet(initialized, 0, nplans * sizeof(bool));

	/*
	 * create new AppendState for our append node
	 */
	appendstate = makeNode(AppendState);
	appendstate->as_whichplan = 0;
	appendstate->as_nplans = nplans;
	appendstate->as_initialized = initialized;

	node->appendstate = appendstate;

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
	ExecInitResultTupleSlot(estate, &appendstate->cstate);

	/*
	 * call ExecInitNode on each of the plans in our list and save the
	 * results into the array "initialized"
	 */
	for (i = 0; i < nplans; i++)
	{
		appendstate->as_whichplan = i;
		exec_append_initialize_next(node);

		initNode = (Plan *) nth(i, appendplans);
		initialized[i] = ExecInitNode(initNode, estate, (Plan *) node);
	}

	/*
	 * initialize tuple type
	 */
	ExecAssignResultTypeFromTL((Plan *) node, &appendstate->cstate);
	appendstate->cstate.cs_ProjInfo = NULL;

	/*
	 * return the result from the first subplan's initialization
	 */
	appendstate->as_whichplan = 0;
	exec_append_initialize_next(node);

	return TRUE;
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
ExecProcAppend(Append *node)
{
	EState	   *estate;
	AppendState *appendstate;
	int			whichplan;
	List	   *appendplans;
	Plan	   *subnode;
	TupleTableSlot *result;
	TupleTableSlot *result_slot;
	ScanDirection direction;

	/*
	 * get information from the node
	 */
	appendstate = node->appendstate;
	estate = node->plan.state;
	direction = estate->es_direction;
	appendplans = node->appendplans;
	whichplan = appendstate->as_whichplan;
	result_slot = appendstate->cstate.cs_ResultTupleSlot;

	/*
	 * figure out which subplan we are currently processing
	 */
	subnode = (Plan *) nth(whichplan, appendplans);

	if (subnode == NULL)
		elog(DEBUG, "ExecProcAppend: subnode is NULL");

	/*
	 * get a tuple from the subplan
	 */
	result = ExecProcNode(subnode, (Plan *) node);

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
			appendstate->as_whichplan++;
		else
			appendstate->as_whichplan--;

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
ExecEndAppend(Append *node)
{
	EState	   *estate;
	AppendState *appendstate;
	int			nplans;
	List	   *appendplans;
	bool	   *initialized;
	int			i;

	/*
	 * get information from the node
	 */
	appendstate = node->appendstate;
	estate = node->plan.state;
	appendplans = node->appendplans;
	nplans = appendstate->as_nplans;
	initialized = appendstate->as_initialized;

	/*
	 * shut down each of the subscans
	 */
	for (i = 0; i < nplans; i++)
	{
		if (initialized[i])
			ExecEndNode((Plan *) nth(i, appendplans), (Plan *) node);
	}
}

void
ExecReScanAppend(Append *node, ExprContext *exprCtxt, Plan *parent)
{
	AppendState *appendstate = node->appendstate;
	int			nplans = length(node->appendplans);
	int			i;

	for (i = 0; i < nplans; i++)
	{
		Plan	   *rescanNode;

		appendstate->as_whichplan = i;
		rescanNode = (Plan *) nth(i, node->appendplans);
		if (rescanNode->chgParam == NULL)
		{
			exec_append_initialize_next(node);
			ExecReScan((Plan *) rescanNode, exprCtxt, (Plan *) node);
		}
	}
	appendstate->as_whichplan = 0;
	exec_append_initialize_next(node);
}
