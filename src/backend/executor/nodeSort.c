/*-------------------------------------------------------------------------
 *
 * nodeSort.c--
 *    Routines to handle sorting of relations into temporaries.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/executor/nodeSort.c,v 1.2 1996/07/30 07:45:35 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "executor/executor.h"
#include "executor/nodeSort.h"
#include "utils/palloc.h"
#include "utils/psort.h"
#include "catalog/catalog.h"
#include "storage/bufmgr.h"
#include "optimizer/internal.h" /* for _TEMP_RELATION_ID_ */

/* ----------------------------------------------------------------
 *    	FormSortKeys(node)
 *    
 *    	Forms the structure containing information used to sort the relation.
 *    
 *    	Returns an array of ScanKeyData.
 * ----------------------------------------------------------------
 */
static ScanKey
FormSortKeys(Sort *sortnode)
{
    ScanKey		sortkeys;
    List    		*targetList;
    List   		*tl;
    int			keycount;
    Resdom    		*resdom;
    AttrNumber 		resno;
    Index   		reskey;
    Oid			reskeyop;
    
    /* ----------------
     *	get information from the node
     * ----------------
     */
    targetList = sortnode->plan.targetlist;
    keycount =   sortnode->keycount;
    
    /* ----------------
     *	first allocate space for scan keys
     * ----------------
     */
    if (keycount <= 0)
	elog(WARN, "FormSortKeys: keycount <= 0");
    sortkeys = (ScanKey) palloc(keycount * sizeof(ScanKeyData));

    /* ----------------  
     *	form each scan key from the resdom info in the target list
     * ----------------
     */
    foreach(tl, targetList) {
	TargetEntry *target = (TargetEntry *)lfirst(tl);
	resdom =  target->resdom;
	resno =   resdom->resno;
	reskey =  resdom->reskey;
	reskeyop = resdom->reskeyop;
	
	if (reskey > 0) {
	    ScanKeyEntryInitialize(&sortkeys[reskey-1],
				   0,
				   resno,
				   (RegProcedure) DatumGetInt32(reskeyop),
				   (Datum) 0);
	}
    }
    
    return sortkeys;
}

/* ----------------------------------------------------------------
 *   	ExecSort
 *
 * old comments
 *   	Retrieves tuples fron the outer subtree and insert them into a 
 *   	temporary relation. The temporary relation is then sorted and
 *   	the sorted relation is stored in the relation whose ID is indicated
 *   	in the 'tempid' field of this node.
 *   	Assumes that heap access method is used.
 *   
 *   	Conditions:
 *   	  -- none.
 *   
 *   	Initial States:
 *   	  -- the outer child is prepared to return the first tuple.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecSort(Sort *node)
{
    EState	   *estate;
    SortState	   *sortstate;
    Plan 	   *outerNode;
    ScanDirection  dir;
    int		   keycount;
    ScanKey	   sortkeys;
    Relation	   tempRelation;
    Relation	   currentRelation;
    HeapScanDesc   currentScanDesc;
    HeapTuple      heapTuple;
    TupleTableSlot *slot;
    Buffer	   buffer;
    int		   tupCount = 0;
    
    /* ----------------
     *	get state info from node
     * ----------------
     */
    SO1_printf("ExecSort: %s\n",
	       "entering routine");
    
    sortstate =   node->sortstate;
    estate =      node->plan.state;
    dir =   	  estate->es_direction;
    
    /* ----------------
     *	the first time we call this, we retrieve all tuples
     *  from the subplan into a temporary relation and then
     *  we sort the relation.  Subsequent calls return tuples
     *  from the temporary relation.
     * ----------------
     */
    
    if (sortstate->sort_Flag == false) {
	SO1_printf("ExecSort: %s\n",
		   "sortstate == false -> sorting subplan");
	/* ----------------
	 *  set all relations to be scanned in the forward direction 
	 *  while creating the temporary relation.
	 * ----------------
	 */
	estate->es_direction = ForwardScanDirection;
	
	/* ----------------
	 *   if we couldn't create the temp or current relations then
	 *   we print a warning and return NULL.
	 * ----------------
	 */
	tempRelation =  sortstate->sort_TempRelation;
	if (tempRelation == NULL) {
	    elog(DEBUG, "ExecSort: temp relation is NULL! aborting...");
	    return NULL;
	}
	
	currentRelation = sortstate->csstate.css_currentRelation;
	if (currentRelation == NULL) {
	    elog(DEBUG, "ExecSort: current relation is NULL! aborting...");
	    return NULL;
	}
	
	/* ----------------
	 *   retrieve tuples from the subplan and
	 *   insert them in the temporary relation
	 * ----------------
	 */
	outerNode = outerPlan((Plan *) node);
	SO1_printf("ExecSort: %s\n",
		   "inserting tuples into tempRelation");
	
	for (;;) {
	    slot = ExecProcNode(outerNode, (Plan*)node);
	    
	    if (TupIsNull(slot))
		break;
	    
	    tupCount++;
	    
	    heapTuple = slot->val;
	    
	    heap_insert(tempRelation, 	/* relation desc */
			heapTuple);	/* heap tuple to insert */
	    
	    ExecClearTuple(slot);
	}
	
	/* ----------------
	 *   now sort the tuples in our temporary relation
	 *   into a new sorted relation using psort()
	 *
	 *   psort() seems to require that the relations
	 *   are created and opened in advance.
	 *   -cim 1/25/90
	 * ----------------
	 */
	keycount = node->keycount;
	sortkeys = (ScanKey)sortstate->sort_Keys;
	SO1_printf("ExecSort: %s\n",
		   "calling psort");
	
	/*
	 * If no tuples were fetched from the proc node return NULL now
	 * psort dumps it if 0 tuples are in the relation and I don't want
	 * to try to debug *that* routine!!
	 */
	if (tupCount == 0)
	    return NULL;
	
	psort(tempRelation,	/* old relation */
	      currentRelation,	/* new relation */
	      keycount,		/* number keys */
	      sortkeys);	/* keys */
	
	if (currentRelation == NULL) {
	    elog(DEBUG, "ExecSort: sorted relation is NULL! aborting...");
	    return NULL;
	}
	
	/* ----------------
	 *   restore to user specified direction
	 * ----------------
	 */
	estate->es_direction = dir;
	
	/* ----------------
	 *   now initialize the scan descriptor to scan the
	 *   sorted relation and update the sortstate information
	 * ----------------
	 */
	currentScanDesc = heap_beginscan(currentRelation,    /* relation */
					 ScanDirectionIsBackward(dir),
					 /* bkwd flag */
					 NowTimeQual,        /* time qual */
					 0, 		  /* num scan keys */
					 NULL);		  /* scan keys */
	
	sortstate->csstate.css_currentRelation = currentRelation;
	sortstate->csstate.css_currentScanDesc = currentScanDesc;
	
	/* ----------------
	 *  make sure the tuple descriptor is up to date
	 * ----------------
	 */
	slot = sortstate->csstate.css_ScanTupleSlot;
	
	slot->ttc_tupleDescriptor = 
	    RelationGetTupleDescriptor(currentRelation);
	
	/* ----------------
	 *  finally set the sorted flag to true
	 * ----------------
	 */
	sortstate->sort_Flag = true;
    }
    else {
	slot =  sortstate->csstate.css_ScanTupleSlot; 
    }
    
    SO1_printf("ExecSort: %s\n",
	       "retrieveing tuple from sorted relation");
    
    /* ----------------
     *	at this point we know we have a sorted relation so
     *  we preform a simple scan on it with amgetnext()..
     * ----------------
     */
    currentScanDesc = sortstate->csstate.css_currentScanDesc;
    
    heapTuple = heap_getnext(currentScanDesc, 	/* scan desc */
			     ScanDirectionIsBackward(dir),
			     /* bkwd flag */
			     &buffer); 		/* return: buffer */
    
    /* Increase the pin count on the buffer page, because the tuple stored in 
       the slot also points to it (as well as the scan descriptor). If we 
       don't, ExecStoreTuple will decrease the pin count on the next iteration.
       - 01/09/93 */
    
    if (buffer != InvalidBuffer) 
        IncrBufferRefCount(buffer);
    
    return ExecStoreTuple(heapTuple,  	/* tuple to store */
			  slot,   	/* slot to store in */
			  buffer,	/* this tuple's buffer */
			  false);	/* don't free stuff from amgetnext */
}

/* ----------------------------------------------------------------
 *   	ExecInitSort
 *
 * old comments
 *   	Creates the run-time state information for the sort node
 *   	produced by the planner and initailizes its outer subtree.
 * ----------------------------------------------------------------
 */
bool
ExecInitSort(Sort *node, EState *estate, Plan *parent)
{
    SortState		*sortstate;
    Plan		*outerPlan;
    ScanKey		sortkeys;
    TupleDesc		tupType;
    Oid			tempOid;
    Oid			sortOid;
    Relation		tempDesc;
    Relation		sortedDesc;
    
    SO1_printf("ExecInitSort: %s\n",
	       "initializing sort node");
    
    /* ----------------
     *  assign the node's execution state
     * ----------------
     */
    node->plan.state = estate;
    
    /* ----------------
     * create state structure
     * ----------------
     */
    sortstate = makeNode(SortState);
    sortstate->sort_Flag = 0;
    sortstate->sort_Keys = NULL;
    sortstate->sort_TempRelation = NULL;

    node->sortstate = sortstate;
    
    /* ----------------
     *  Miscellanious initialization
     *
     *	     +	assign node's base_id
     *       +	assign debugging hooks
     *
     *  Sort nodes don't initialize their ExprContexts because
     *  they never call ExecQual or ExecTargetList.
     * ----------------
     */
    ExecAssignNodeBaseInfo(estate, &sortstate->csstate.cstate, parent);
    
#define SORT_NSLOTS 1
    /* ----------------
     *	tuple table initialization
     *
     *  sort nodes only return scan tuples from their sorted
     *  relation.
     * ----------------
     */
      ExecInitScanTupleSlot(estate, &sortstate->csstate);  
      ExecInitResultTupleSlot(estate, &sortstate->csstate.cstate); 
    
    /* ----------------
     * initializes child nodes
     * ----------------
     */
    outerPlan = outerPlan((Plan *) node);
    ExecInitNode(outerPlan, estate, (Plan *) node);
    
    /* ----------------
     *	initialize sortstate information
     * ----------------
     */
    sortkeys = FormSortKeys(node);
    sortstate->sort_Keys = sortkeys;
    sortstate->sort_Flag = false;
    
    /* ----------------
     * 	initialize tuple type.  no need to initialize projection
     *  info because this node doesn't do projections.
     * ----------------
     */
    ExecAssignScanTypeFromOuterPlan((Plan *) node, &sortstate->csstate); 
    sortstate->csstate.cstate.cs_ProjInfo = NULL;
    
    /* ----------------
     *	get type information needed for ExecCreatR
     * ----------------
     */
    tupType = ExecGetScanType(&sortstate->csstate); 
    
    /* ----------------
     *	ExecCreatR wants its second argument to be an object id of
     *  a relation in the range table or _TEMP_RELATION_ID_
     *  indicating that the relation is not in the range table.
     *
     *  In the second case ExecCreatR creates a temp relation.
     *  (currently this is the only case we support -cim 10/16/89)
     * ----------------
     */
    tempOid = 	node->tempid;
    sortOid =	_TEMP_RELATION_ID_;
    
    /* ----------------
     *	create the temporary relations
     * ----------------
     */
/*    len = 		ExecTargetListLength(node->plan.targetlist); */
    tempDesc = 		ExecCreatR(tupType, tempOid);
    sortedDesc = 	ExecCreatR(tupType, sortOid);
    
    /* ----------------
     *	save the relation descriptor in the sortstate
     * ----------------
     */
    sortstate->sort_TempRelation = tempDesc;
    sortstate->csstate.css_currentRelation = sortedDesc;
    SO1_printf("ExecInitSort: %s\n",
	       "sort node initialized");
    
    /* ----------------
     *  return relation oid of temporary sort relation in a list
     *	(someday -- for now we return LispTrue... cim 10/12/89)
     * ----------------
     */
    return TRUE;
}

int
ExecCountSlotsSort(Sort *node)
{
    return ExecCountSlotsNode(outerPlan((Plan *)node)) +
	ExecCountSlotsNode(innerPlan((Plan *)node)) +
	    SORT_NSLOTS;
}

/* ----------------------------------------------------------------
 *   	ExecEndSort(node)
 *
 * old comments
 *   	destroys the temporary relation.
 * ----------------------------------------------------------------
 */
void
ExecEndSort(Sort *node)
{
    SortState	*sortstate;
    Relation	tempRelation;
    Relation	sortedRelation;
    Plan	*outerPlan;
    
    /* ----------------
     *	get info from the sort state 
     * ----------------
     */
    SO1_printf("ExecEndSort: %s\n",
	       "shutting down sort node");
    
    sortstate =      node->sortstate;
    tempRelation =   sortstate->sort_TempRelation;
    sortedRelation = sortstate->csstate.css_currentRelation;
    
    heap_destroyr(tempRelation);
    heap_destroyr(sortedRelation);

    
    /* ----------------
     *	close the sorted relation and shut down the scan.
     * ----------------
     */
    ExecCloseR((Plan *) node);
    
    /* ----------------
     *	shut down the subplan
     * ----------------
     */
    outerPlan = outerPlan((Plan *) node);
    ExecEndNode(outerPlan, (Plan*)node);
    
    /* ----------------
     *	clean out the tuple table
     * ----------------
     */
    ExecClearTuple(sortstate->csstate.css_ScanTupleSlot); 
    
    SO1_printf("ExecEndSort: %s\n",
	       "sort node shutdown");
} 

/* ----------------------------------------------------------------
 *	ExecSortMarkPos
 * ----------------------------------------------------------------
 */
void
ExecSortMarkPos(Sort *node)
{
    SortState 	 *sortstate;
    HeapScanDesc sdesc;
    
    /* ----------------
     *	if we haven't sorted yet, just return
     * ----------------
     */
    sortstate =   node->sortstate;
    if (sortstate->sort_Flag == false)
	return; 
    
    sdesc = sortstate->csstate.css_currentScanDesc;
    heap_markpos(sdesc);
    return;
}

/* ----------------------------------------------------------------
 *	ExecSortRestrPos
 * ----------------------------------------------------------------
 */
void
ExecSortRestrPos(Sort *node)
{
    SortState	 *sortstate;
    HeapScanDesc sdesc;
    
    /* ----------------
     *	if we haven't sorted yet, just return.
     * ----------------
     */
    sortstate =  node->sortstate;
    if (sortstate->sort_Flag == false)
	return;
    
    /* ----------------
     *	restore the scan to the previously marked position
     * ----------------
     */
    sdesc = sortstate->csstate.css_currentScanDesc;
    heap_restrpos(sdesc);
}
