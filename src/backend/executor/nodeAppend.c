/*-------------------------------------------------------------------------
 *
 * nodeAppend.c--
 *    routines to handle append nodes.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/executor/nodeAppend.c,v 1.5 1997/08/19 21:31:07 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/* INTERFACE ROUTINES
 *      ExecInitAppend  - initialize the append node
 *      ExecProcAppend  - retrieve the next tuple from the node
 *      ExecEndAppend   - shut down the append node
 *
 *   NOTES
 *      Each append node contains a list of one or more subplans which
 *      must be iteratively processed (forwards or backwards).
 *      Tuples are retrieved by executing the 'whichplan'th subplan
 *      until the subplan stops returning tuples, at which point that
 *      plan is shut down and the next started up.
 *
 *      Append nodes don't make use of their left and right
 *      subtrees, rather they maintain a list of subplans so
 *      a typical append node looks like this in the plan tree:
 *
 *                 ...
 *                 /
 *              Append -------+------+------+--- nil
 *              /   \         |      |      |
 *            nil   nil      ...    ...    ...
 *                               subplans
 *
 *      Append nodes are currently used to support inheritance
 *      queries, where several relations need to be scanned.
 *      For example, in our standard person/student/employee/student-emp
 *      example, where student and employee inherit from person
 *      and student-emp inherits from student and employee, the
 *      query:
 *
 *              retrieve (e.name) from e in person*
 *
 *      generates the plan:
 *
 *                |
 *              Append -------+-------+--------+--------+
 *              /   \         |       |        |        |
 *            nil   nil      Scan    Scan     Scan     Scan
 *                            |       |        |        |
 *                          person employee student student-emp
 */
#include "postgres.h"


#include "access/heapam.h"
#include "executor/executor.h"
#include "executor/execdebug.h"
#include "executor/nodeAppend.h"
#include "executor/nodeIndexscan.h"
#include "utils/palloc.h"
#include "utils/mcxt.h"
#include "parser/parsetree.h"		/* for rt_store() macro */

static bool exec_append_initialize_next(Append *node);

/* ----------------------------------------------------------------
 *      exec-append-initialize-next
 *    
 *      Sets up the append node state (i.e. the append state node)
 *      for the "next" scan.
 *    
 *      Returns t iff there is a "next" scan to process.
 * ----------------------------------------------------------------
 */
static bool
exec_append_initialize_next(Append *node)
{
    EState         *estate;
    AppendState    *unionstate;
    TupleTableSlot *result_slot;
    List           *rangeTable;
    
    int            whichplan;
    int            nplans;
    List           *rtentries;
    ResTarget      *rtentry;
    
    Index          unionrelid;
    
    /* ----------------
     *  get information from the append node
     * ----------------
     */
    estate = node->plan.state;
    unionstate = node->unionstate;
    result_slot = unionstate->cstate.cs_ResultTupleSlot;
    rangeTable = estate->es_range_table;
    
    whichplan = unionstate->as_whichplan;
    nplans =    unionstate->as_nplans;
    rtentries = node->unionrtentries;
    
    if (whichplan < 0) {
        /* ----------------
         *      if scanning in reverse, we start at
         *      the last scan in the list and then
         *      proceed back to the first.. in any case
         *      we inform ExecProcAppend that we are
         *      at the end of the line by returning FALSE
         * ----------------
         */
        unionstate->as_whichplan = 0;
        return FALSE;
	
    } else if (whichplan >= nplans) {
        /* ----------------
         *      as above, end the scan if we go beyond
         *      the last scan in our list..
         * ----------------
         */
        unionstate->as_whichplan = nplans - 1;
        return FALSE;
        
    } else {
        /* ----------------
         *      initialize the scan
         *      (and update the range table appropriately)
         *        (doesn't this leave the range table hosed for anybody upstream
	 *         of the Append node??? - jolly )
         * ----------------
         */
	if (node->unionrelid > 0) {
	    rtentry = nth(whichplan, rtentries);
	    if (rtentry == NULL)
		elog(DEBUG, "exec_append_initialize_next: rtentry is nil");
	    
	    unionrelid = node->unionrelid;

	    rt_store(unionrelid, rangeTable, rtentry);

	    if (unionstate->as_junkFilter_list) {
		estate->es_junkFilter =
		  (JunkFilter*)nth(whichplan, 
				   unionstate->as_junkFilter_list);
	    }
	    if (unionstate->as_result_relation_info_list) {
		estate->es_result_relation_info =
		  (RelationInfo*) nth(whichplan, 
				      unionstate->as_result_relation_info_list);
	    }
	    result_slot->ttc_whichplan = whichplan;
	}
	
	return TRUE;
    }
}

/* ----------------------------------------------------------------
 *      ExecInitAppend
 *    
 *      Begins all of the subscans of the append node, storing the
 *      scan structures in the 'initialized' vector of the append-state
 *      structure.
 *
 *     (This is potentially wasteful, since the entire result of the
 *      append node may not be scanned, but this way all of the
 *      structures get allocated in the executor's top level memory
 *      block instead of that of the call to ExecProcAppend.)
 *    
 *      Returns the scan result of the first scan.
 * ----------------------------------------------------------------
 */
bool
ExecInitAppend(Append *node, EState *estate, Plan *parent)
{
    AppendState *unionstate;
    int         nplans;
    List        *resultList = NULL;
    List        *rtentries;
    List        *unionplans;
    bool        *initialized;
    int         i;
    Plan        *initNode;
    List        *junkList;
    RelationInfo *es_rri = estate->es_result_relation_info;
    
    /* ----------------
     *  assign execution state to node and get information
     *  for append state
     * ----------------
     */
    node->plan.state = estate;
    
    unionplans = node->unionplans;
    nplans = length(unionplans);
    rtentries = node->unionrtentries;
    
    CXT1_printf("ExecInitAppend: context is %d\n", CurrentMemoryContext);
    initialized = (bool *)palloc(nplans * sizeof(bool));
    
    /* ----------------
     *  create new AppendState for our append node
     * ----------------
     */   
    unionstate = makeNode(AppendState);
    unionstate->as_whichplan = 0;
    unionstate->as_nplans = nplans;
    unionstate->as_initialized = initialized;
    unionstate->as_rtentries = rtentries;

    node->unionstate = unionstate;
    
    /* ----------------
     *  Miscellanious initialization
     *
     *	     +	assign node's base_id
     *       +	assign debugging hooks
     *
     *  Append plans don't have expression contexts because they
     *  never call ExecQual or ExecTargetList.
     * ----------------
     */
    ExecAssignNodeBaseInfo(estate, &unionstate->cstate, parent);
    
#define APPEND_NSLOTS 1
    /* ----------------
     *	append nodes still have Result slots, which hold pointers
     *  to tuples, so we have to initialize them..
     * ----------------
     */
    ExecInitResultTupleSlot(estate, &unionstate->cstate);
    
    /*
     * If the inherits rtentry is the result relation, we have to make
     * a result relation info list for all inheritors so we can update
     * their indices and put the result tuples in the right place etc.
     *
     * e.g. replace p (age = p.age + 1) from p in person*
     */
    if ((es_rri != (RelationInfo*)NULL) &&
	(node->unionrelid == es_rri->ri_RangeTableIndex))
	{
	    RelationInfo *rri;
	    List         *rtentryP;
	    
	    foreach(rtentryP,rtentries)
		{
		    Oid reloid;
		    RangeTblEntry *rtentry = lfirst(rtentryP);
		    
		    reloid = rtentry->relid;
 		    rri = makeNode(RelationInfo); 
		    rri->ri_RangeTableIndex = es_rri->ri_RangeTableIndex;
		    rri->ri_RelationDesc = heap_open(reloid);
		    rri->ri_NumIndices = 0;
		    rri->ri_IndexRelationDescs = NULL; /* index descs */
		    rri->ri_IndexRelationInfo = NULL;  /* index key info */

 		    resultList = lcons(rri,resultList);
		    ExecOpenIndices(reloid, rri);
		}
	    unionstate->as_result_relation_info_list = resultList;
	}
    /* ----------------
     *  call ExecInitNode on each of the plans in our list
     *  and save the results into the array "initialized"
     * ----------------
     */       
    junkList = NIL;

    for(i = 0; i < nplans ; i++ ) {
	JunkFilter *j;
	List       *targetList;
        /* ----------------
         *  NOTE: we first modify range table in 
         *        exec_append_initialize_next() and
         *        then initialize the subnode,
         *        since it may use the range table.
         * ----------------
         */
        unionstate->as_whichplan = i;
        exec_append_initialize_next(node);

        initNode = (Plan *) nth(i, unionplans);
        initialized[i] =  ExecInitNode(initNode, estate, (Plan*) node);
	
	/* ---------------
	 *  Each targetlist in the subplan may need its own junk filter
	 *
	 *  This is true only when the reln being replaced/deleted is
	 *  the one that we're looking at the subclasses of
	 * ---------------
	 */
	if ((es_rri != (RelationInfo*)NULL) &&
	    (node->unionrelid == es_rri->ri_RangeTableIndex)) {
	    
	    targetList = initNode->targetlist;
	    j = (JunkFilter *) ExecInitJunkFilter(targetList);
	    junkList = lappend(junkList, j);
	}
	
    }
    unionstate->as_junkFilter_list = junkList;
    if (junkList != NIL)
	estate->es_junkFilter = (JunkFilter *)lfirst(junkList);
    
    /* ----------------
     *	initialize the return type from the appropriate subplan.
     * ----------------
     */
    initNode = (Plan *) nth(0, unionplans);
    ExecAssignResultType(&unionstate->cstate,
/*			 ExecGetExecTupDesc(initNode), */
			 ExecGetTupType(initNode));
    unionstate->cstate.cs_ProjInfo = NULL;
    
    /* ----------------
     *  return the result from the first subplan's initialization
     * ----------------
     */       
    unionstate->as_whichplan = 0;
    exec_append_initialize_next(node);
#if 0
    result = (List *) initialized[0];
#endif    
    return TRUE;
}

int
ExecCountSlotsAppend(Append *node)
{
    List *plan;
    List *unionplans = node->unionplans;
    int  nSlots     = 0;
    
    foreach (plan,unionplans) {
	nSlots += ExecCountSlotsNode((Plan *)lfirst(plan));
    }
    return nSlots + APPEND_NSLOTS;
}

/* ----------------------------------------------------------------
 *     ExecProcAppend
 *    
 *      Handles the iteration over the multiple scans.
 *    
 *     NOTE: Can't call this ExecAppend, that name is used in execMain.l
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecProcAppend(Append *node)
{
    EState              *estate;
    AppendState         *unionstate;
    
    int                 whichplan;
    List                *unionplans;
    Plan                *subnode;
    TupleTableSlot      *result;
    TupleTableSlot      *result_slot;
    ScanDirection       direction;
    
    /* ----------------
     *  get information from the node
     * ----------------
     */
    unionstate =      node->unionstate;
    estate = 	      node->plan.state;
    direction =       estate->es_direction;
    
    unionplans =      node->unionplans;
    whichplan =       unionstate->as_whichplan;
    result_slot =     unionstate->cstate.cs_ResultTupleSlot;
    
    /* ----------------
     *  figure out which subplan we are currently processing
     * ----------------
     */
    subnode = (Plan *) nth(whichplan, unionplans);
    
    if (subnode == NULL)
        elog(DEBUG, "ExecProcAppend: subnode is NULL");
    
    /* ----------------
     *  get a tuple from the subplan
     * ----------------
     */
    result = ExecProcNode(subnode, (Plan*)node);
    
    if (! TupIsNull(result)) {
        /* ----------------
         *  if the subplan gave us something then place a copy of
	 *  whatever we get into our result slot and return it, else..
         * ----------------
         */
	return ExecStoreTuple(result->val,
			      result_slot, result->ttc_buffer, false);
	
    } else {
        /* ----------------
         *  .. go on to the "next" subplan in the appropriate
         *  direction and try processing again (recursively)
         * ----------------
         */
        whichplan = unionstate->as_whichplan;
        
        if (ScanDirectionIsForward(direction))
	    {
		unionstate->as_whichplan = whichplan + 1;
	    }
        else
	    {
		unionstate->as_whichplan = whichplan - 1;
	    }
	
	/* ----------------
	 *  return something from next node or an empty slot
	 *  all of our subplans have been exhausted.
	 * ----------------
	 */
        if (exec_append_initialize_next(node)) {
	    ExecSetSlotDescriptorIsNew(result_slot, true);
            return
		ExecProcAppend(node);
        } else
	    return ExecClearTuple(result_slot);
    }
}

/* ----------------------------------------------------------------
 *      ExecEndAppend
 *    
 *      Shuts down the subscans of the append node.
 *    
 *      Returns nothing of interest.
 * ----------------------------------------------------------------
 */
void
ExecEndAppend(Append *node)
{
    AppendState *unionstate;
    int         nplans;
    List        *unionplans;
    bool     	*initialized;
    int         i;
    List        *resultRelationInfoList;
    RelationInfo    *resultRelationInfo;

    /* ----------------
     *  get information from the node
     * ----------------
     */
    unionstate =  node->unionstate;
    unionplans =  node->unionplans;
    nplans =      unionstate->as_nplans;
    initialized = unionstate->as_initialized;
    
    /* ----------------
     *  shut down each of the subscans
     * ----------------
     */
    for(i = 0; i < nplans; i++) {
        if (initialized[i]==TRUE) {
            ExecEndNode( (Plan *) nth(i, unionplans), (Plan*)node );
        }
    }
    
    /* ----------------
     *  close out the different result relations
     * ----------------
     */
    resultRelationInfoList = unionstate->as_result_relation_info_list;
    while (resultRelationInfoList != NIL) {
      Relation resultRelationDesc;
	
      resultRelationInfo = (RelationInfo*) lfirst(resultRelationInfoList);
      resultRelationDesc = resultRelationInfo->ri_RelationDesc;
      heap_close(resultRelationDesc);
      pfree(resultRelationInfo);
      resultRelationInfoList = lnext(resultRelationInfoList);
    }
    if (unionstate->as_result_relation_info_list)
      pfree(unionstate->as_result_relation_info_list);

  /* XXX should free unionstate->as_rtentries  and unionstate->as_junkfilter_list here  */
}

