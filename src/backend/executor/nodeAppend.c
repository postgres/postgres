/*-------------------------------------------------------------------------
 *
 * nodeAppend.c
 *	  routines to handle append nodes.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeAppend.c,v 1.36 2000/10/05 19:11:26 tgl Exp $
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
 *		Append nodes are currently used for unions, and to support inheritance
 *		queries, where several relations need to be scanned.
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
	TupleTableSlot *result_slot;
	List	   *rangeTable;
	int			whichplan;
	int			nplans;
	List	   *inheritrtable;
	RangeTblEntry *rtentry;

	/* ----------------
	 *	get information from the append node
	 * ----------------
	 */
	estate = node->plan.state;
	appendstate = node->appendstate;
	result_slot = appendstate->cstate.cs_ResultTupleSlot;
	rangeTable = estate->es_range_table;

	whichplan = appendstate->as_whichplan;
	nplans = appendstate->as_nplans;
	inheritrtable = node->inheritrtable;

	if (whichplan < 0)
	{
		/* ----------------
		 *		if scanning in reverse, we start at
		 *		the last scan in the list and then
		 *		proceed back to the first.. in any case
		 *		we inform ExecProcAppend that we are
		 *		at the end of the line by returning FALSE
		 * ----------------
		 */
		appendstate->as_whichplan = 0;
		return FALSE;

	}
	else if (whichplan >= nplans)
	{
		/* ----------------
		 *		as above, end the scan if we go beyond
		 *		the last scan in our list..
		 * ----------------
		 */
		appendstate->as_whichplan = nplans - 1;
		return FALSE;

	}
	else
	{
		/* ----------------
		 *		initialize the scan
		 *		(and update the range table appropriately)
		 *
		 *		(doesn't this leave the range table hosed for anybody upstream
		 *		 of the Append node??? - jolly )
		 * ----------------
		 */
		if (node->inheritrelid > 0)
		{
			rtentry = nth(whichplan, inheritrtable);
			Assert(rtentry != NULL);
			rt_store(node->inheritrelid, rangeTable, rtentry);
		}

		if (appendstate->as_junkFilter_list)
		{
			estate->es_junkFilter = (JunkFilter *) nth(whichplan,
										appendstate->as_junkFilter_list);
		}
		if (appendstate->as_result_relation_info_list)
		{
			estate->es_result_relation_info = (RelationInfo *) nth(whichplan,
							  appendstate->as_result_relation_info_list);
		}
		result_slot->ttc_whichplan = whichplan;

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
	List	   *inheritrtable;
	List	   *appendplans;
	bool	   *initialized;
	int			i;
	Plan	   *initNode;
	List	   *junkList;
	RelationInfo *es_rri = estate->es_result_relation_info;
	bool		inherited_result_rel = false;

	CXT1_printf("ExecInitAppend: context is %d\n", CurrentMemoryContext);

	/* ----------------
	 *	assign execution state to node and get information
	 *	for append state
	 * ----------------
	 */
	node->plan.state = estate;

	appendplans = node->appendplans;
	nplans = length(appendplans);
	inheritrtable = node->inheritrtable;

	initialized = (bool *) palloc(nplans * sizeof(bool));
	MemSet(initialized, 0, nplans * sizeof(bool));

	/* ----------------
	 *	create new AppendState for our append node
	 * ----------------
	 */
	appendstate = makeNode(AppendState);
	appendstate->as_whichplan = 0;
	appendstate->as_nplans = nplans;
	appendstate->as_initialized = initialized;

	node->appendstate = appendstate;

	/* ----------------
	 *	Miscellaneous initialization
	 *
	 *	Append plans don't have expression contexts because they
	 *	never call ExecQual or ExecProject.
	 * ----------------
	 */

#define APPEND_NSLOTS 1
	/* ----------------
	 *	append nodes still have Result slots, which hold pointers
	 *	to tuples, so we have to initialize them.
	 * ----------------
	 */
	ExecInitResultTupleSlot(estate, &appendstate->cstate);

	/*
	 * If the inherits rtentry is the result relation, we have to make a
	 * result relation info list for all inheritors so we can update their
	 * indices and put the result tuples in the right place etc.
	 *
	 * e.g. replace p (age = p.age + 1) from p in person*
	 */
	if ((es_rri != (RelationInfo *) NULL) &&
		(node->inheritrelid == es_rri->ri_RangeTableIndex))
	{
		List	   *resultList = NIL;
		Oid			initial_reloid = RelationGetRelid(es_rri->ri_RelationDesc);
		List	   *rtentryP;

		inherited_result_rel = true;

		foreach(rtentryP, inheritrtable)
		{
			RangeTblEntry *rtentry = lfirst(rtentryP);
			Oid			reloid = rtentry->relid;
			RelationInfo *rri;

			/*
			 * We must recycle the RelationInfo already opened by InitPlan()
			 * for the parent rel, else we will leak the associated relcache
			 * refcount. 
			 */
			if (reloid == initial_reloid)
			{
				Assert(es_rri != NULL);	/* check we didn't use it already */
				rri = es_rri;
				es_rri = NULL;
			}
			else
			{
				rri = makeNode(RelationInfo);
				rri->ri_RangeTableIndex = node->inheritrelid;
				rri->ri_RelationDesc = heap_open(reloid, RowExclusiveLock);
				rri->ri_NumIndices = 0;
				rri->ri_IndexRelationDescs = NULL;	/* index descs */
				rri->ri_IndexRelationInfo = NULL;	/* index key info */

				/*
				 * XXX if the operation is a DELETE then we need not open
				 * indices, but how to tell that here?
				 */
				if (rri->ri_RelationDesc->rd_rel->relhasindex)
					ExecOpenIndices(rri);
			}

			/*
			 * NB: the as_result_relation_info_list must be in the same
			 * order as the rtentry list otherwise update or delete on
			 * inheritance hierarchies won't work.
			 */
			resultList = lappend(resultList, rri);
		}

		appendstate->as_result_relation_info_list = resultList;
		/* Check that we recycled InitPlan()'s RelationInfo */
		Assert(es_rri == NULL);
		/* Just for paranoia's sake, clear link until we set it properly */
		estate->es_result_relation_info = NULL;
	}

	/* ----------------
	 *	call ExecInitNode on each of the plans in our list
	 *	and save the results into the array "initialized"
	 * ----------------
	 */
	junkList = NIL;

	for (i = 0; i < nplans; i++)
	{
		/* ----------------
		 *	NOTE: we first modify range table in
		 *		  exec_append_initialize_next() and
		 *		  then initialize the subnode,
		 *		  since it may use the range table.
		 * ----------------
		 */
		appendstate->as_whichplan = i;
		exec_append_initialize_next(node);

		initNode = (Plan *) nth(i, appendplans);
		initialized[i] = ExecInitNode(initNode, estate, (Plan *) node);

		/* ---------------
		 *	Each targetlist in the subplan may need its own junk filter
		 *
		 *	This is true only when the reln being replaced/deleted is
		 *	the one that we're looking at the subclasses of
		 * ---------------
		 */
		if (inherited_result_rel)
		{
			JunkFilter *j = ExecInitJunkFilter(initNode->targetlist,
											   ExecGetTupType(initNode));

			junkList = lappend(junkList, j);
		}

	}
	appendstate->as_junkFilter_list = junkList;
	if (junkList != NIL)
		estate->es_junkFilter = (JunkFilter *) lfirst(junkList);

	/* ----------------
	 *	initialize the return type from the appropriate subplan.
	 * ----------------
	 */
	initNode = (Plan *) nth(0, appendplans);
	ExecAssignResultType(&appendstate->cstate, ExecGetTupType(initNode));
	appendstate->cstate.cs_ProjInfo = NULL;

	/* ----------------
	 *	return the result from the first subplan's initialization
	 * ----------------
	 */
	appendstate->as_whichplan = 0;
	exec_append_initialize_next(node);

	return TRUE;
}

int
ExecCountSlotsAppend(Append *node)
{
	List	   *plan;
	List	   *appendplans = node->appendplans;
	int			nSlots = 0;

	foreach(plan, appendplans)
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

	/* ----------------
	 *	get information from the node
	 * ----------------
	 */
	appendstate = node->appendstate;
	estate = node->plan.state;
	direction = estate->es_direction;
	appendplans = node->appendplans;
	whichplan = appendstate->as_whichplan;
	result_slot = appendstate->cstate.cs_ResultTupleSlot;

	/* ----------------
	 *	figure out which subplan we are currently processing
	 * ----------------
	 */
	subnode = (Plan *) nth(whichplan, appendplans);

	if (subnode == NULL)
		elog(DEBUG, "ExecProcAppend: subnode is NULL");

	/* ----------------
	 *	get a tuple from the subplan
	 * ----------------
	 */
	result = ExecProcNode(subnode, (Plan *) node);

	if (!TupIsNull(result))
	{
		/* ----------------
		 *	if the subplan gave us something then place a copy of
		 *	whatever we get into our result slot and return it.
		 *
		 *	Note we rely on the subplan to retain ownership of the
		 *	tuple for as long as we need it --- we don't copy it.
		 * ----------------
		 */
		return ExecStoreTuple(result->val, result_slot, InvalidBuffer, false);
	}
	else
	{
		/* ----------------
		 *	.. go on to the "next" subplan in the appropriate
		 *	direction and try processing again (recursively)
		 * ----------------
		 */
		whichplan = appendstate->as_whichplan;

		if (ScanDirectionIsForward(direction))
			appendstate->as_whichplan = whichplan + 1;
		else
			appendstate->as_whichplan = whichplan - 1;

		/* ----------------
		 *	return something from next node or an empty slot
		 *	all of our subplans have been exhausted.
		 * ----------------
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
	List	   *resultRelationInfoList;

	/* ----------------
	 *	get information from the node
	 * ----------------
	 */
	appendstate = node->appendstate;
	estate = node->plan.state;
	appendplans = node->appendplans;
	nplans = appendstate->as_nplans;
	initialized = appendstate->as_initialized;

	/* ----------------
	 *	shut down each of the subscans
	 * ----------------
	 */
	for (i = 0; i < nplans; i++)
	{
		if (initialized[i])
			ExecEndNode((Plan *) nth(i, appendplans), (Plan *) node);
	}

	/* ----------------
	 *	close out the different result relations
	 * ----------------
	 */
	resultRelationInfoList = appendstate->as_result_relation_info_list;
	while (resultRelationInfoList != NIL)
	{
		RelationInfo *resultRelationInfo;
		Relation	resultRelationDesc;

		resultRelationInfo = (RelationInfo *) lfirst(resultRelationInfoList);
		resultRelationDesc = resultRelationInfo->ri_RelationDesc;
		heap_close(resultRelationDesc, NoLock);
		pfree(resultRelationInfo);
		resultRelationInfoList = lnext(resultRelationInfoList);
	}
	appendstate->as_result_relation_info_list = NIL;
	/*
	 * This next step is critical to prevent EndPlan() from trying to close
	 * an already-closed-and-deleted RelationInfo --- es_result_relation_info
	 * is pointing at one of the nodes we just zapped above.
	 */
	estate->es_result_relation_info = NULL;

	/*
	 * XXX should free appendstate->as_junkfilter_list here
	 */
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
