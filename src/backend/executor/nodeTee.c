/*-------------------------------------------------------------------------
 *
 * nodeTee.c--
 *    
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *   DESCRIPTION
 *      This code provides support for a tee node, which allows multiple
 *    parent in a megaplan. 
 *      
 *   INTERFACE ROUTINES
 *      ExecTee	
 *	ExecInitTee
 *	ExecEndTee
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/executor/Attic/nodeTee.c,v 1.6 1997/07/28 00:54:11 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <sys/types.h>
#include <sys/file.h>
#include "postgres.h"

#include "utils/palloc.h"
#include "utils/relcache.h" 
#include "utils/mcxt.h"
#include "storage/bufmgr.h"  /* for IncrBufferRefCount */
#include "storage/smgr.h"
#include "optimizer/internal.h"
#include "executor/executor.h"
#include "executor/nodeTee.h"
#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "tcop/pquery.h"
#include "access/heapam.h"

/* ------------------------------------------------------------------
 *	ExecInitTee
 *
 *	Create tee state
 *
 * ------------------------------------------------------------------
 */
bool
ExecInitTee(Tee* node, EState *currentEstate, Plan * parent)
{
    TeeState      *teeState;
    Plan          *outerPlan;
    int           len;
    Relation      bufferRel;
    TupleDesc     tupType;
    EState        *estate;
    
    /* it is possible that the Tee has already been initialized
       since it can be reached by multiple parents.
       If it is already initialized, simply return and do 
       not initialize the children nodes again
    */
    if (node->plan.state)
      return TRUE; 

    /* ----------------
     *  assign the node's execution state
     * ----------------
     */
    /* make a new executor state, because we have a different
       es_range_table */

/*     node->plan.state = estate;*/

    estate = CreateExecutorState();
    estate->es_direction = currentEstate->es_direction;
    estate->es_BaseId = currentEstate->es_BaseId;
    estate->es_BaseId = currentEstate->es_BaseId;
    estate->es_tupleTable = currentEstate->es_tupleTable;
    estate->es_refcount = currentEstate->es_refcount;
    estate->es_junkFilter = currentEstate->es_junkFilter;

    /* use the range table for Tee subplan since the range tables
       for the two parents may be different */
    if (node->rtentries)
	estate->es_range_table = node->rtentries; 
    else
	estate->es_range_table = currentEstate->es_range_table;

    node->plan.state = estate;


    /* ----------------
     * create teeState structure
     * ----------------
     */
    teeState = makeNode(TeeState);
    teeState->tee_leftPlace = 0;
    teeState->tee_rightPlace = 0;
    teeState->tee_lastPlace = 0;
    teeState->tee_bufferRel = NULL;
    teeState->tee_leftScanDesc = NULL;
    teeState->tee_rightScanDesc = NULL;


    node->teestate = teeState;
    
    /* ----------------
     *  Miscellanious initialization
     *
     *	     +	assign node's base_id
     *       +	assign debugging hooks and
     *       +	create expression context for node
     * ----------------
     */
    ExecAssignNodeBaseInfo(estate, &(teeState->cstate), parent);
    ExecAssignExprContext(estate, &(teeState->cstate));

#define TEE_NSLOTS 2
    /* ----------------
     *	initialize tuple slots
     * ----------------
     */
    ExecInitResultTupleSlot(estate, &(teeState->cstate));
    
    /* initialize child nodes */
    outerPlan = outerPlan((Plan*) node);
    ExecInitNode(outerPlan, estate, (Plan*) node);

    /* ----------------
     *  the tuple type info is from the outer plan of this node
     *  the result type is also the same as the outerplan  
     */
    ExecAssignResultTypeFromOuterPlan((Plan*) node, &(teeState->cstate));
    ExecAssignProjectionInfo((Plan*)node, &teeState->cstate);
    
    /* ---------------------------------------
       initialize temporary relation to buffer tuples 
    */
    tupType = ExecGetResultType(&(teeState->cstate));
    len =     ExecTargetListLength(((Plan*)node)->targetlist);

/*    bufferRel = ExecCreatR(len, tupType, _TEMP_RELATION_ID_);  */

    /* create a catalogued relation even though this is a temporary relation */
    /* cleanup of catalogued relations is easier to do */
    
    if (node->teeTableName[0] != '\0') {
	Relation r;

	teeState->tee_bufferRelname = pstrdup(node->teeTableName);

	/* we are given an tee table name, 
	   if a relation by that name exists, then we open it,
	   else we create it and then open it */
	r = RelationNameGetRelation(teeState->tee_bufferRelname);

	if (RelationIsValid(r))
	    bufferRel = heap_openr(teeState->tee_bufferRelname);
	else
	    bufferRel = heap_open(heap_create(teeState->tee_bufferRelname,
/*FIX */				      NULL,
				    'n',
				    DEFAULT_SMGR,
				    tupType));
    }
    else {
	sprintf(teeState->tee_bufferRelname,
		"ttemp_%d", /* 'ttemp' for 'tee' temporary*/
		newoid()); 
/*	bufferRel = ExecCreatR(len, tupType, _TEMP_RELATION_ID); */
	    bufferRel = heap_open(heap_create(teeState->tee_bufferRelname,
					      NULL, /*XXX */
					      'n',
					      DEFAULT_SMGR,
					      tupType));
    }

    teeState->tee_bufferRel = bufferRel;

    /*initialize a memory context for allocating thing like scan descriptors */
    /* we do this so that on cleanup of the tee, we can free things.
       if we didn't have our own memory context, we would be in the memory
       context of the portal that we happen to be using at the moment */

    teeState->tee_mcxt = (MemoryContext)CreateGlobalMemory(teeState->tee_bufferRelname);

    /* don't initialize the scan descriptors here
       because it's not good to initialize scan descriptors on empty
       rels. Wait until the scan descriptors are needed
       before initializing them. */
    
    teeState->tee_leftScanDesc = NULL;
    teeState->tee_rightScanDesc = NULL;
    
    return TRUE;
}

int 
ExecCountSlotsTee(Tee *node)
{
  /* Tee nodes can't have innerPlans */
    return ExecCountSlotsNode(outerPlan(node)) + TEE_NSLOTS;
}

/* ----------------------------------------------------------------
   initTeeScanDescs
      initializes the left and right scandescs on the temporary
      relation of a Tee node

      must open two separate scan descriptors,
      because the left and right scans may be at different points
* ----------------------------------------------------------------
*/
static void 
initTeeScanDescs(Tee* node)
{
  TeeState *teeState;
  Relation bufferRel;
  ScanDirection dir;
  MemoryContext       orig;

  teeState = node->teestate;
  if (teeState->tee_leftScanDesc && teeState->tee_rightScanDesc)  
    return;

  orig = CurrentMemoryContext;
  MemoryContextSwitchTo(teeState->tee_mcxt);

  bufferRel = teeState->tee_bufferRel;
  dir = ((Plan*)node)->state->es_direction; /* backwards not handled yet XXX */

  if (teeState->tee_leftScanDesc == NULL)
    {
      teeState->tee_leftScanDesc = heap_beginscan(bufferRel,
						  ScanDirectionIsBackward(dir),
						  NowTimeQual, /* time qual */
						  0,       /* num scan keys */
						  NULL    /* scan keys */
						  );
    }
  if (teeState->tee_rightScanDesc == NULL)
    {
      teeState->tee_rightScanDesc = heap_beginscan(bufferRel,
						  ScanDirectionIsBackward(dir),
						  NowTimeQual, /* time qual */
						  0,     /* num scan keys */
						  NULL  /* scan keys */
						  );
    }

    MemoryContextSwitchTo(orig);
}

/* ----------------------------------------------------------------
 *	ExecTee(node)
 *
 *
 *      A Tee serves to connect a subplan to multiple parents.
 *      the subplan is always the outplan of the Tee node.
 *      
 *      The Tee gets requests from either leftParent or rightParent,
 *      fetches the result tuple from the child, and then
 *      stored the result into a temporary relation (serving as a queue).
 *      leftPlace and rightPlace keep track of where the left and rightParents
 *      are.
 *      If a parent requests a tuple and that parent is not at the end 
 *      of the temporary relation, then the request is satisfied from
 *      the queue instead of by executing the child plan
 *
 * ----------------------------------------------------------------
 */

TupleTableSlot*
ExecTee(Tee *node, Plan *parent)
{
    EState 		*estate;
    TeeState            *teeState;
    int                 leftPlace, rightPlace, lastPlace;
    int                 branch;
    TupleTableSlot*     result;
    TupleTableSlot*     slot;
    Plan                *childNode;
    ScanDirection       dir;
    HeapTuple           heapTuple;
    Relation            bufferRel;
    HeapScanDesc        scanDesc;
    Buffer              buffer;

    estate = ((Plan*)node)->state;
    teeState = node->teestate;
    leftPlace = teeState->tee_leftPlace;
    rightPlace = teeState->tee_rightPlace;
    lastPlace = teeState->tee_lastPlace;
    bufferRel = teeState->tee_bufferRel;

    childNode = outerPlan(node);

    dir = estate->es_direction;

    /* XXX doesn't handle backwards direction yet */

    if (parent == node->leftParent) {
	branch = leftPlace;
      }
    else
      if ( (parent == node->rightParent) || (parent == (Plan*) node)) 
	  /* the tee node could be the root node of the plan,
	     in which case, we treat it like a right-parent pull*/
	{
	branch = rightPlace;
      }
    else
      {
	elog(WARN,"A Tee node can only be executed from its left or right parent\n");
	return NULL;
      }

    if (branch == lastPlace)
      { /* we're at the end of the queue already,
	   - get a new tuple from the child plan,
	   - store it in the queue,
	   - increment lastPlace,
	   - increment leftPlace or rightPlace as appropriate,
	   - and return result
	   */
	slot = ExecProcNode(childNode, (Plan*)node);
	if (!TupIsNull(slot)) 
	  {
	    heapTuple = slot->val;
	    
	    /* insert into temporary relation */
	    heap_insert(bufferRel, heapTuple);
	    
	    /* once there is data in the temporary relation,
	       ensure that the left and right scandescs are initialized */
	    initTeeScanDescs(node);

	    scanDesc = (parent == node->leftParent) ?
	      teeState->tee_leftScanDesc : teeState->tee_rightScanDesc;

	    {
      /* move the scandesc forward so we don't re-read this tuple later */
	      HeapTuple throwAway;
	      /* Buffer buffer;*/
	      throwAway = heap_getnext(scanDesc,
				       ScanDirectionIsBackward(dir),
	             		   /*  &buffer */
				       (Buffer*)NULL);
	    }

	    /* set the shouldFree field of the child's slot so that
	       when the child's slot is free'd, this tuple isn't free'd also */
	    /* does this mean this tuple has to be garbage collected later??*/
	    slot->ttc_shouldFree = false;

	    teeState->tee_lastPlace = lastPlace + 1;
	  }
	result = slot;
      }	
    else 
      {/* the desired data already exists in the temporary relation */ 
	scanDesc = (parent == node->leftParent) ?
	  teeState->tee_leftScanDesc : teeState->tee_rightScanDesc;

	heapTuple = heap_getnext(scanDesc,
				 ScanDirectionIsBackward(dir),
				 &buffer);

	/* Increase the pin count on the buffer page, because the
	   tuple stored in the slot also points to it (as well as
	   the scan descriptor). If we don't, ExecStoreTuple will
	   decrease the pin count on the next iteration. */
    
	if (buffer != InvalidBuffer) 
	  IncrBufferRefCount(buffer);
    
	slot = teeState->cstate.cs_ResultTupleSlot;
	slot->ttc_tupleDescriptor = RelationGetTupleDescriptor(bufferRel);

	result =   ExecStoreTuple(heapTuple,/* tuple to store */
				  slot,  /* slot to store in */
				  buffer,/* this tuple's buffer */
				  false); /* don't free stuff from heap_getnext */
				 
      }

    if (parent == node->leftParent)
      {
	teeState->tee_leftPlace = leftPlace+1;
      }
    else
      {
	teeState->tee_rightPlace = rightPlace+1;
      }

    return result;
}

/* ----------------------------------------------------------------
 *   	ExecTeeReScan(node)
 *   
 *   	Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecTeeReScan(Tee *node, ExprContext *exprCtxt, Plan *parent)
{

    EState 		*estate;
    TeeState            *teeState;
    ScanDirection       dir;

    estate = ((Plan*)node)->state;
    teeState = node->teestate;

    dir = estate->es_direction;
    
    /* XXX doesn't handle backwards direction yet */

    if (parent == node->leftParent) {
      if (teeState->tee_leftScanDesc)
	{
	  heap_rescan(teeState->tee_leftScanDesc,
		      ScanDirectionIsBackward(dir),
		      NULL);
	  teeState->tee_leftPlace = 0;
	}
    }
    else
      {
	if (teeState->tee_rightScanDesc)
	  {
	  heap_rescan(teeState->tee_leftScanDesc,
		      ScanDirectionIsBackward(dir),
		      NULL);
	  teeState->tee_rightPlace = 0;
	  }
      }
}


/* ---------------------------------------------------------------------
 *	ExecEndTee
 *
 *   End the Tee node, and free up any storage
 * since a Tee node can be downstream of multiple parent nodes,
 * only free when both parents are done
 * --------------------------------------------------------------------
 */

void 
ExecEndTee(Tee* node, Plan* parent)
{
    EState 		*estate;
    TeeState            *teeState;
    int                 leftPlace, rightPlace, lastPlace;
    Relation            bufferRel;
    MemoryContext       orig;

    estate = ((Plan*)node)->state;
    teeState = node->teestate;
    leftPlace = teeState->tee_leftPlace;
    rightPlace = teeState->tee_rightPlace;
    lastPlace = teeState->tee_lastPlace;

    if (!node->leftParent || parent == node->leftParent)
	leftPlace = -1;

    if (!node->rightParent || parent == node->rightParent)
	rightPlace = -1;

    if (parent == (Plan*)node) 
      rightPlace = leftPlace = -1;

    teeState->tee_leftPlace = leftPlace;
    teeState->tee_rightPlace = rightPlace;
    if ( (leftPlace == -1) && (rightPlace == -1) )
      {
	/* remove the temporary relations */
	/* and close the scan descriptors */

	bufferRel = teeState->tee_bufferRel;
        if (bufferRel) {
	    heap_destroyr(bufferRel);
	  teeState->tee_bufferRel = NULL;
	  if (teeState->tee_mcxt) {
	    orig = CurrentMemoryContext;
	    MemoryContextSwitchTo(teeState->tee_mcxt);
	  }
	  else
		orig = 0;

	  if (teeState->tee_leftScanDesc)
	    {
	    heap_endscan(teeState->tee_leftScanDesc);
	    teeState->tee_leftScanDesc = NULL;
	  }
	  if (teeState->tee_rightScanDesc)
	    {
	    heap_endscan(teeState->tee_rightScanDesc);
	    teeState->tee_rightScanDesc = NULL;
	  }

	  if (teeState->tee_mcxt) {
	    MemoryContextSwitchTo(orig);
	    teeState->tee_mcxt = NULL;
	  }
	}
     }
    
}

