/*-------------------------------------------------------------------------
 *
 * nodeIndexscan.c--
 *    Routines to support indexes and indexed scans of relations
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/executor/nodeIndexscan.c,v 1.7 1997/03/12 20:58:26 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *	ExecInsertIndexTuples	inserts tuples into indices on result relation
 *
 *   	ExecIndexScan 		scans a relation using indices
 *   	ExecIndexNext 		using index to retrieve next tuple
 *   	ExecInitIndexScan	creates and initializes state info. 
 *   	ExecIndexReScan		rescans the indexed relation.
 *   	ExecEndIndexScan 	releases all storage.
 *   	ExecIndexMarkPos	marks scan position.
 *   	ExecIndexRestrPos	restores scan position.
 *
 *   NOTES
 *	the code supporting ExecInsertIndexTuples should be
 *	collected and merged with the genam stuff.
 *
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/execdebug.h"
#include "executor/nodeIndexscan.h"

#include "optimizer/clauses.h"	/* for get_op, get_leftop, get_rightop */
#include "parser/parsetree.h"	/* for rt_fetch() */

#include "access/skey.h"
#include "access/heapam.h"
#include "access/genam.h"
#include "utils/palloc.h"
#include "utils/mcxt.h"
#include "catalog/index.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "nodes/nodeFuncs.h"

/* ----------------
 *	Misc stuff to move to executor.h soon -cim 6/5/90
 * ----------------
 */
#define NO_OP		0
#define LEFT_OP		1
#define RIGHT_OP	2

static TupleTableSlot *IndexNext(IndexScan *node);

/* ----------------------------------------------------------------
 *   	IndexNext
 *
 *	Retrieve a tuple from the IndexScan node's currentRelation
 *	using the indices in the IndexScanState information.
 *
 *	note: the old code mentions 'Primary indices'.  to my knowledge
 *	we only support a single secondary index. -cim 9/11/89
 *
 * old comments:
 *   	retrieve a tuple from relation using the indices given.
 *   	The indices are used in the order they appear in 'indices'.
 *   	The indices may be primary or secondary indices:
 *   	  * primary index -- 	scan the relation 'relID' using keys supplied.
 *   	  * secondary index -- 	scan the index relation to get the 'tid' for
 *   			       	a tuple in the relation 'relID'.
 *   	If the current index(pointed by 'indexPtr') fails to return a
 *   	tuple, the next index in the indices is used.
 *   
 *        bug fix so that it should retrieve on a null scan key.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
IndexNext(IndexScan *node)
{
    EState	   		*estate;
    CommonScanState   		*scanstate;
    IndexScanState 		*indexstate;
    ScanDirection  		direction;
    int		   		indexPtr;
    IndexScanDescPtr 		scanDescs;
    IndexScanDesc  		scandesc;
    Relation	   		heapRelation;
    RetrieveIndexResult  	result;
    ItemPointer	   		iptr;
    HeapTuple	   		tuple;
    TupleTableSlot		*slot;
    Buffer			buffer = InvalidBuffer;

    /* ----------------
     *	extract necessary information from index scan node
     * ----------------
     */
    estate =     	node->scan.plan.state;
    direction =  	estate->es_direction;
    scanstate =  	node->scan.scanstate;
    indexstate = 	node->indxstate;
    indexPtr =   	indexstate->iss_IndexPtr;
    scanDescs = 	indexstate->iss_ScanDescs;
    scandesc =  	scanDescs[ indexPtr ];
    heapRelation =	scanstate->css_currentRelation;

    slot = scanstate->css_ScanTupleSlot;

    /* ----------------
     *	ok, now that we have what we need, fetch an index tuple.
     * ----------------
     */

    for(;;) {
	result = index_getnext(scandesc, direction);
	/* ----------------
	 *  if scanning this index succeeded then return the
	 *  appropriate heap tuple.. else return NULL.
	 * ----------------
	 */
	if (result) {
	    iptr =  &result->heap_iptr;
	    tuple = heap_fetch(heapRelation,
			       NowTimeQual,
			       iptr,
			       &buffer);
	    /* be tidy */
	    pfree(result);
	    
	    if (tuple == NULL) {
		/* ----------------
		 *   we found a deleted tuple, so keep on scanning..
		 * ----------------
		 */
		if (BufferIsValid(buffer))
		    ReleaseBuffer(buffer);
		continue;
	    }
	    
	    /* ----------------
	     *	store the scanned tuple in the scan tuple slot of
	     *  the scan state.  Eventually we will only do this and not
	     *  return a tuple.  Note: we pass 'false' because tuples
	     *  returned by amgetnext are pointers onto disk pages and
	     *  were not created with palloc() and so should not be pfree()'d.
	     * ----------------
	     */
	    ExecStoreTuple(tuple,	  /* tuple to store */
			   slot, 	  /* slot to store in */
			   buffer, 	  /* buffer associated with tuple  */
			   false);   	  /* don't pfree */
	    
	    return slot;
	}
	
	/* ----------------
	 *  if we get here it means the index scan failed so we
	 *  are at the end of the scan..
	 * ----------------
	 */
	return ExecClearTuple(slot);
    }
}

/* ----------------------------------------------------------------
 *	ExecIndexScan(node)
 *
 * old comments:
 *   	Scans the relation using primary or secondary indices and returns 
 *         the next qualifying tuple in the direction specified.
 *   	It calls ExecScan() and passes it the access methods which returns
 *   	the next tuple using the indices.
 *   	
 *   	Conditions:
 *   	  -- the "cursor" maintained by the AMI is positioned at the tuple
 *   	     returned previously.
 *   
 *   	Initial States:
 *   	  -- the relation indicated is opened for scanning so that the 
 *   	     "cursor" is positioned before the first qualifying tuple.
 *   	  -- all index realtions are opened for scanning.
 *   	  -- indexPtr points to the first index.
 *   	  -- state variable ruleFlag = nil.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecIndexScan(IndexScan *node)
{
    TupleTableSlot *returnTuple;

    /* ----------------
     *	use IndexNext as access method
     * ----------------
     */
    returnTuple = ExecScan(&node->scan, IndexNext);
    return returnTuple;
}	

/* ----------------------------------------------------------------
 *    	ExecIndexReScan(node)
 *
 *	Recalculates the value of the scan keys whose value depends on 
 *	information known at runtime and rescans the indexed relation.
 *	Updating the scan key was formerly done separately in 
 * 	ExecUpdateIndexScanKeys. Integrating it into ReScan
 *	makes rescans of indices and
 *	relations/general streams more uniform.
 *
 * ----------------------------------------------------------------
 */
void
ExecIndexReScan(IndexScan *node, ExprContext *exprCtxt, Plan* parent)
{
    EState	    *estate;
    IndexScanState  *indexstate;
    ScanDirection   direction;
    IndexScanDescPtr scanDescs;
    ScanKey	    *scanKeys;
    IndexScanDesc   sdesc;
    ScanKey	    skey;
    int		    numIndices;
    int 	    i;

    Pointer		*runtimeKeyInfo;
    int		    	indexPtr;
    int	    		*numScanKeys;
    List	    	*indxqual;
    List		*qual;
    int 		n_keys;
    ScanKey		scan_keys;
    int			*run_keys;
    int			j;
    Expr		*clause;
    Node		*scanexpr;
    Datum		scanvalue;
    bool		isNull;
    bool		isDone;

    indexstate = node->indxstate;
    estate = node->scan.plan.state;
    direction = estate->es_direction;
    indexstate = node->indxstate;
    numIndices = indexstate->iss_NumIndices;
    scanDescs = indexstate->iss_ScanDescs;
    scanKeys = indexstate->iss_ScanKeys;

    runtimeKeyInfo = (Pointer *) indexstate->iss_RuntimeKeyInfo;

    if (runtimeKeyInfo != NULL) {
	/* 
	 * get the index qualifications and
	 * recalculate the appropriate values
	 */
	indexPtr = indexstate->iss_IndexPtr;
	indxqual = node->indxqual;
	qual = nth(indexPtr, indxqual);
	numScanKeys = indexstate->iss_NumScanKeys;
	n_keys = numScanKeys[indexPtr];
	run_keys = (int *) runtimeKeyInfo[indexPtr];
	scan_keys = (ScanKey) scanKeys[indexPtr];

	for (j=0; j < n_keys; j++) {
	    /* 
	     * If we have a run-time key, then extract the run-time
	     * expression and evaluate it with respect to the current
	     * outer tuple.  We then stick the result into the scan
	     * key.
	     */
	    if (run_keys[j] != NO_OP) {
		clause =    nth(j, qual);
		scanexpr =  (run_keys[j] == RIGHT_OP) ?
		    (Node*) get_rightop(clause) : (Node*) get_leftop(clause) ;
		/* pass in isDone but ignore it.  We don't iterate in quals */
		scanvalue = (Datum)
		    ExecEvalExpr(scanexpr, exprCtxt, &isNull, &isDone);
		scan_keys[j].sk_argument = scanvalue;
		if (isNull) {
		    scan_keys[j].sk_flags |= SK_ISNULL;
		} else {
		    scan_keys[j].sk_flags &= ~SK_ISNULL;
		}
	    }
	}
    } 

    /* 
     * rescans all indices
     *
     * note: AMrescan assumes only one scan key.  This may have
     *	     to change if we ever decide to support multiple keys.
     */
    for (i = 0; i < numIndices; i++) {
	sdesc = scanDescs[ i ];
	skey =  scanKeys[ i ];
	index_rescan(sdesc, direction, skey);
    }

    /* ----------------
     *	perhaps return something meaningful
     * ----------------
     */
    return;
}

/* ----------------------------------------------------------------
 *    	ExecEndIndexScan
 *
 * old comments
 *    	Releases any storage allocated through C routines.
 *    	Returns nothing.
 * ----------------------------------------------------------------
 */
void
ExecEndIndexScan(IndexScan *node)
{
    CommonScanState	*scanstate;
    IndexScanState  	*indexstate;
    ScanKey	    	*scanKeys;
    int		    	numIndices;
    int 	        i;

    scanstate =  node->scan.scanstate;
    indexstate = node->indxstate;

    /* ----------------
     *	extract information from the node
     * ----------------
     */
    numIndices = indexstate->iss_NumIndices;
    scanKeys =   indexstate->iss_ScanKeys;

    /* ----------------
     *	Free the projection info and the scan attribute info
     *
     *  Note: we don't ExecFreeResultType(scanstate) 
     *        because the rule manager depends on the tupType
     *	      returned by ExecMain().  So for now, this
     *	      is freed at end-transaction time.  -cim 6/2/91     
     * ----------------
     */    
    ExecFreeProjectionInfo(&scanstate->cstate);

    /* ----------------
     *	close the heap and index relations
     * ----------------
     */
    ExecCloseR((Plan *) node);

    /* ----------------
     *	free the scan keys used in scanning the indices
     * ----------------
     */
    for (i=0; i<numIndices; i++) { 
	if (scanKeys[i]!=NULL)
	    pfree(scanKeys[i]);
	    
    }

    /* ----------------
     *	clear out tuple table slots
     * ----------------
     */
    ExecClearTuple(scanstate->cstate.cs_ResultTupleSlot);
    ExecClearTuple(scanstate->css_ScanTupleSlot);
/*    ExecClearTuple(scanstate->css_RawTupleSlot); */
}

/* ----------------------------------------------------------------
 *    	ExecIndexMarkPos
 *
 * old comments
 *    	Marks scan position by marking the current index.
 *    	Returns nothing.
 * ----------------------------------------------------------------
 */
void
ExecIndexMarkPos(IndexScan *node)
{
    IndexScanState	*indexstate;
    IndexScanDescPtr	indexScanDescs;
    IndexScanDesc	scanDesc;
    int			indexPtr;

    indexstate =     node->indxstate;
    indexPtr =       indexstate->iss_IndexPtr;
    indexScanDescs = indexstate->iss_ScanDescs;
    scanDesc = indexScanDescs[ indexPtr ];

    /* ----------------
     *	XXX access methods don't return marked positions so
     * ----------------
     */
    IndexScanMarkPosition( scanDesc );
    return;
}

/* ----------------------------------------------------------------
 *    	ExecIndexRestrPos
 *
 * old comments
 *    	Restores scan position by restoring the current index.
 *    	Returns nothing.
 *    
 *    	XXX Assumes previously marked scan position belongs to current index
 * ----------------------------------------------------------------
 */
void
ExecIndexRestrPos(IndexScan *node)
{
    IndexScanState	*indexstate;
    IndexScanDescPtr	indexScanDescs;
    IndexScanDesc	scanDesc;
    int			indexPtr;

    indexstate =     node->indxstate;
    indexPtr =       indexstate->iss_IndexPtr;
    indexScanDescs = indexstate->iss_ScanDescs;
    scanDesc = indexScanDescs[ indexPtr ];

    IndexScanRestorePosition( scanDesc );
}

/* ----------------------------------------------------------------
 *    	ExecInitIndexScan
 *
 *	Initializes the index scan's state information, creates
 *	scan keys, and opens the base and index relations.
 *
 *	Note: index scans have 2 sets of state information because
 *	      we have to keep track of the base relation and the
 *	      index relations.
 *	      
 * old comments
 *    	Creates the run-time state information for the node and 
 *    	sets the relation id to contain relevant decriptors.
 *    
 *    	Parameters: 
 *    	  node: IndexNode node produced by the planner.
 *	  estate: the execution state initialized in InitPlan.
 * ----------------------------------------------------------------
 */
bool
ExecInitIndexScan(IndexScan *node, EState *estate, Plan *parent)
{
    IndexScanState  	*indexstate;
    CommonScanState	*scanstate;
    List	    	*indxqual;
    List	    	*indxid;
    int			i;
    int		    	numIndices;
    int		    	indexPtr;
    ScanKey	    	*scanKeys;
    int	    		*numScanKeys;
    RelationPtr	    	relationDescs;
    IndexScanDescPtr	scanDescs;
    Pointer		*runtimeKeyInfo;
    bool		have_runtime_keys;
    List	   	*rangeTable;
    RangeTblEntry    	*rtentry;
    Index	    	relid;
    Oid	   		reloid;
    TimeQual		timeQual;

    Relation	    	currentRelation;
    HeapScanDesc    	currentScanDesc;
    ScanDirection   	direction;
    int			baseid;

    /* ----------------
     *	assign execution state to node
     * ----------------
     */
    node->scan.plan.state = estate;

    /* --------------------------------
     *  Part 1)  initialize scan state
     *
     *	create new CommonScanState for node
     * --------------------------------
     */
    scanstate = makeNode(CommonScanState);
/*
    scanstate->ss_ProcOuterFlag = false;
    scanstate->ss_OldRelId = 0;
*/

    node->scan.scanstate = scanstate;

    /* ----------------
     *	assign node's base_id .. we don't use AssignNodeBaseid() because
     *  the increment is done later on after we assign the index scan's
     *  scanstate.  see below. 
     * ----------------
     */
    baseid = estate->es_BaseId;
/*    scanstate->csstate.cstate.bnode.base_id = baseid; */
    scanstate->cstate.cs_base_id = baseid;

    /* ----------------
     *  create expression context for node
     * ----------------
     */
    ExecAssignExprContext(estate, &scanstate->cstate);

#define INDEXSCAN_NSLOTS 3
    /* ----------------
     *	tuple table initialization
     * ----------------
     */
    ExecInitResultTupleSlot(estate, &scanstate->cstate); 
    ExecInitScanTupleSlot(estate, scanstate);
/*    ExecInitRawTupleSlot(estate, scanstate); */

    /* ----------------
     * 	initialize projection info.  result type comes from scan desc
     *  below..
     * ----------------
     */
    ExecAssignProjectionInfo((Plan *) node, &scanstate->cstate);

   /* --------------------------------
     *  Part 2)  initialize index scan state
     *
     *	create new IndexScanState for node
     * --------------------------------
     */
    indexstate = makeNode(IndexScanState);
    indexstate->iss_NumIndices = 0;
    indexstate->iss_IndexPtr = 0;
    indexstate->iss_ScanKeys = NULL;
    indexstate->iss_NumScanKeys = NULL;
    indexstate->iss_RuntimeKeyInfo = NULL;
    indexstate->iss_RelationDescs = NULL;
    indexstate->iss_ScanDescs = NULL;
    
    node->indxstate = indexstate;

    /* ----------------
     *	assign base id to index scan state also
     * ----------------
     */
    indexstate->cstate.cs_base_id = baseid;
    baseid++;
    estate->es_BaseId = baseid;

    /* ----------------
     *	get the index node information
     * ----------------
     */
    indxid = 	node->indxid;
    indxqual = 	node->indxqual;
    numIndices = length(indxid);
    indexPtr = 	 0;

    CXT1_printf("ExecInitIndexScan: context is %d\n", CurrentMemoryContext);

    /* ----------------
     *	scanKeys is used to keep track of the ScanKey's. This is needed
     *	because a single scan may use several indices and each index has
     *  its own ScanKey.
     * ----------------
     */
    numScanKeys = (int *) palloc(numIndices * sizeof(int));
    scanKeys = (ScanKey *) palloc(numIndices * sizeof(ScanKey));
    relationDescs = (RelationPtr) palloc(numIndices * sizeof(Relation));
    scanDescs =  (IndexScanDescPtr) palloc(numIndices * sizeof(IndexScanDesc));

    /* ----------------
     *	initialize runtime key info.
     * ----------------
     */
    have_runtime_keys = false;
    runtimeKeyInfo = (Pointer *)
	palloc(numIndices * sizeof(Pointer));

    /* ----------------
     *	build the index scan keys from the index qualification
     * ----------------
     */
    for (i=0; i < numIndices; i++) {
	int 		j;
	List		*qual;
	int 		n_keys;
	ScanKey		scan_keys;
	int		*run_keys;
	
	qual =		nth(i, indxqual);
	n_keys = 	length(qual);
	scan_keys = (n_keys <= 0) ? NULL :
	    (ScanKey)palloc(n_keys * sizeof(ScanKeyData));
	run_keys = (n_keys <= 0) ? NULL :
		(int *)palloc(n_keys * sizeof(int));
	
	CXT1_printf("ExecInitIndexScan: context is %d\n",
		    CurrentMemoryContext);
	
	/* ----------------
	 *  for each opclause in the given qual,
	 *  convert each qual's opclause into a single scan key
	 * ----------------
	 */
	for (j=0; j < n_keys; j++) {
	    Expr	*clause;		/* one part of index qual */
	    Oper	*op;		/* operator used in scan.. */
	    Node	*leftop;		/* expr on lhs of operator */
	    Node	*rightop; 	/* expr on rhs ... */
	    bits16	flags = 0;
	    
	    int		scanvar; 	/* which var identifies varattno */
	    AttrNumber	varattno = 0; 	/* att number used in scan */
	    Oid		opid;		/* operator id used in scan */
	    Datum	scanvalue = 0; 	/* value used in scan (if const) */
	    
	    /* ----------------
	     *	extract clause information from the qualification
	     * ----------------
	     */
	    clause = 	nth(j, qual);
	    
	    op = (Oper*)clause->oper;
	    if (!IsA(op,Oper))
		elog(WARN, "ExecInitIndexScan: op not an Oper!");
	    
	    opid = op->opid;
	    
	    /* ----------------
	     *  Here we figure out the contents of the index qual.
	     *  The usual case is (op var const) or (op const var)
	     *  which means we form a scan key for the attribute
	     *  listed in the var node and use the value of the const.
	     *
	     *  If we don't have a const node, then it means that
	     *  one of the var nodes refers to the "scan" tuple and
	     *  is used to determine which attribute to scan, and the
	     *  other expression is used to calculate the value used in
	     *  scanning the index.
	     *
	     *  This means our index scan's scan key is a function of
	     *  information obtained during the execution of the plan
	     *  in which case we need to recalculate the index scan key
	     *  at run time.
	     *  
	     *  Hence, we set have_runtime_keys to true and then set
	     *  the appropriate flag in run_keys to LEFT_OP or RIGHT_OP.
	     *  The corresponding scan keys are recomputed at run time.
	     * ----------------
	     */
	    
	    scanvar = NO_OP;
	    
	    /* ----------------
	     *	determine information in leftop
	     * ----------------
	     */
	    leftop = 	(Node*) get_leftop(clause);
	    
	    if (IsA(leftop,Var) && var_is_rel((Var*)leftop)) {
		/* ----------------
		 *  if the leftop is a "rel-var", then it means
		 *  that it is a var node which tells us which
		 *  attribute to use for our scan key.
		 * ----------------
		 */
		varattno = 	((Var*) leftop)->varattno;
		scanvar = 	LEFT_OP;
	    } else if (IsA(leftop,Const)) {
		/* ----------------
		 *  if the leftop is a const node then it means
		 *  it identifies the value to place in our scan key.
		 * ----------------
		 */
		run_keys[ j ] = NO_OP;
		scanvalue = ((Const*) leftop)->constvalue;
#ifdef INDEXSCAN_PATCH
	    } else if (IsA(leftop,Param)) {
	        bool isnull;
		/* ----------------
		 *  if the leftop is a Param node then it means
		 *  it identifies the value to place in our scan key.
		 * ----------------
		 */
		run_keys[ j ] = NO_OP;
		scanvalue = ExecEvalParam((Param*) leftop, 
				scanstate->cstate.cs_ExprContext,
				&isnull);
		if ( isnull )
		    flags |= SK_ISNULL;
#endif
	    } else if (leftop != NULL &&
		       is_funcclause(leftop) &&
		       var_is_rel(lfirst(((Expr*)leftop)->args))) {
		/* ----------------
		 *  if the leftop is a func node then it means
		 *  it identifies the value to place in our scan key.
		 *  Since functional indices have only one attribute
		 *  the attno must always be set to 1.
		 * ----------------
		 */
		varattno = 	1;
		scanvar = 	LEFT_OP;
		
	    } else {
		/* ----------------
		 *  otherwise, the leftop contains information usable
		 *  at runtime to figure out the value to place in our
		 *  scan key.
		 * ----------------
		 */
		have_runtime_keys = true;
		run_keys[ j ] = LEFT_OP;
		scanvalue = Int32GetDatum((int32) true);
	    }
	    
	    /* ----------------
	     *	now determine information in rightop
	     * ----------------
	     */
	    rightop = 	(Node*) get_rightop(clause);
	    
	    if (IsA(rightop,Var) && var_is_rel((Var*)rightop)) {
		/* ----------------
		 *  here we make sure only one op identifies the
		 *  scan-attribute...
		 * ----------------
		 */
		if (scanvar == LEFT_OP)
		    elog(WARN, "ExecInitIndexScan: %s",
			 "both left and right op's are rel-vars");
		
		/* ----------------
		 *  if the rightop is a "rel-var", then it means
		 *  that it is a var node which tells us which
		 *  attribute to use for our scan key.
		 * ----------------
		 */
		varattno = 	((Var*) rightop)->varattno;
		scanvar = 	RIGHT_OP;
		
	    } else if (IsA(rightop,Const)) {
		/* ----------------
		 *  if the leftop is a const node then it means
		 *  it identifies the value to place in our scan key.
		 * ----------------
		 */
		run_keys[ j ] = NO_OP;
		scanvalue = ((Const*) rightop)->constvalue;
#ifdef INDEXSCAN_PATCH
	    } else if (IsA(rightop,Param)) {
	        bool isnull;
		/* ----------------
		 *  if the rightop is a Param node then it means
		 *  it identifies the value to place in our scan key.
		 * ----------------
		 */
		run_keys[ j ] = NO_OP;
		scanvalue = ExecEvalParam((Param*) rightop, 
				scanstate->cstate.cs_ExprContext,
				&isnull);
		if ( isnull )
		    flags |= SK_ISNULL;
#endif
	    } else if (rightop!=NULL &&
		       is_funcclause(rightop) &&
		       var_is_rel(lfirst(((Expr*)rightop)->args))) {
		/* ----------------
		 *  if the rightop is a func node then it means
		 *  it identifies the value to place in our scan key.
		 *  Since functional indices have only one attribute
		 *  the attno must always be set to 1.
		 * ----------------
		 */
		if (scanvar == LEFT_OP)
		    elog(WARN, "ExecInitIndexScan: %s",
			 "both left and right ops are rel-vars");
		
		varattno = 	1;
		scanvar = 	RIGHT_OP;
		
	    } else {
		/* ----------------
		 *  otherwise, the leftop contains information usable
		 *  at runtime to figure out the value to place in our
		 *  scan key.
		 * ----------------
		 */
		have_runtime_keys = true;
		run_keys[ j ] = RIGHT_OP;
		scanvalue = Int32GetDatum((int32) true);
	    }
	    
	    /* ----------------
	     *	now check that at least one op tells us the scan
	     *  attribute...
	     * ----------------
	     */
	    if (scanvar == NO_OP) 
		elog(WARN, "ExecInitIndexScan: %s",
		     "neither leftop nor rightop refer to scan relation");
	    
	    /* ----------------
	     *	initialize the scan key's fields appropriately
	     * ----------------
	     */
	    ScanKeyEntryInitialize(&scan_keys[j],
				   flags,
				   varattno, /* attribute number to scan */
				   (RegProcedure) opid,	/* reg proc to use */
				   (Datum) scanvalue);	/* constant */
	}
	
	/* ----------------
	 *  store the key information into our array.
	 * ----------------
	 */
	numScanKeys[ i ] = 	n_keys;
	scanKeys[ i ] = 	scan_keys;
	runtimeKeyInfo[ i ] =   (Pointer) run_keys;
    }

    indexstate->iss_NumIndices = numIndices;
    indexstate->iss_IndexPtr = indexPtr;
    indexstate->iss_ScanKeys = scanKeys;
    indexstate->iss_NumScanKeys = numScanKeys;

    /* ----------------
     *	If all of our keys have the form (op var const) , then we have no
     *  runtime keys so we store NULL in the runtime key info.
     *  Otherwise runtime key info contains an array of pointers
     *  (one for each index) to arrays of flags (one for each key)
     *  which indicate that the qual needs to be evaluated at runtime.
     *  -cim 10/24/89
     * ----------------
     */
    if (have_runtime_keys)
	{
	    indexstate->iss_RuntimeKeyInfo = (Pointer) runtimeKeyInfo;
	}
    else {
	indexstate->iss_RuntimeKeyInfo = NULL;
	for (i=0; i < numIndices; i++) {
	    List *qual;
	    int n_keys;
	    qual = nth(i, indxqual);
	    n_keys = length(qual);
	    if (n_keys > 0)
	        pfree(runtimeKeyInfo[i]);
	}
	pfree(runtimeKeyInfo);
    }

    /* ----------------
     *	get the range table and direction information
     *  from the execution state (these are needed to
     *  open the relations).
     * ----------------
     */
    rangeTable = estate->es_range_table;
    direction =  estate->es_direction;

    /* ----------------
     *	open the base relation
     * ----------------
     */
    relid =   node->scan.scanrelid;
    rtentry = rt_fetch(relid, rangeTable);
    reloid =  rtentry->relid;
    timeQual = rtentry->timeQual;

    ExecOpenScanR(reloid,	      /* relation */
		  0,		      /* nkeys */
		  (ScanKey) NULL,     /* scan key */
		  0,		      /* is index */
		  direction,          /* scan direction */
		  timeQual,	      /* time qual */
		  &currentRelation,   /* return: rel desc */
		  (Pointer *) &currentScanDesc);  /* return: scan desc */

    scanstate->css_currentRelation = currentRelation;
    scanstate->css_currentScanDesc = currentScanDesc;


    /* ----------------
     *	get the scan type from the relation descriptor.
     * ----------------
     */
    ExecAssignScanType(scanstate, RelationGetTupleDescriptor(currentRelation));
    ExecAssignResultTypeFromTL((Plan *) node, &scanstate->cstate);

    /* ----------------
     *	index scans don't have subtrees..
     * ----------------
     */
/*    scanstate->ss_ProcOuterFlag = false; */

    /* ----------------
     *	open the index relations and initialize
     *  relation and scan descriptors.
     * ----------------
     */
    for (i=0; i < numIndices; i++) {
	Oid 	indexOid;
	
	indexOid =  (Oid)nthi(i, indxid);
	
	if (indexOid != 0) {
	    ExecOpenScanR(indexOid, 		  /* relation */
			  numScanKeys[ i ],	  /* nkeys */
			  scanKeys[ i ],	  /* scan key */
			  true,			  /* is index */
			  direction,		  /* scan direction */
			  timeQual, 		  /* time qual */
			  &(relationDescs[ i ]),  /* return: rel desc */
			  (Pointer *) &(scanDescs[ i ]));
	    /* return: scan desc */
	}
    }

    indexstate->iss_RelationDescs = relationDescs;
    indexstate->iss_ScanDescs = scanDescs;

    indexstate->cstate.cs_TupFromTlist = false;

    /* ----------------
     *	all done.
     * ----------------
     */
    return TRUE;
}

int
ExecCountSlotsIndexScan(IndexScan *node)
{
    return ExecCountSlotsNode(outerPlan((Plan *)node)) +
	ExecCountSlotsNode(innerPlan((Plan *)node)) +
	    INDEXSCAN_NSLOTS;
}
