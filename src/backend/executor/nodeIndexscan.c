/*-------------------------------------------------------------------------
 *
 * nodeIndexscan.c
 *	  Routines to support indexed scans of relations
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeIndexscan.c,v 1.104.2.1 2005/11/22 18:23:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecIndexScan			scans a relation using indices
 *		ExecIndexNext			using index to retrieve next tuple
 *		ExecInitIndexScan		creates and initializes state info.
 *		ExecIndexReScan			rescans the indexed relation.
 *		ExecEndIndexScan		releases all storage.
 *		ExecIndexMarkPos		marks scan position.
 *		ExecIndexRestrPos		restores scan position.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "executor/execdebug.h"
#include "executor/nodeIndexscan.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "parser/parsetree.h"
#include "utils/memutils.h"


static TupleTableSlot *IndexNext(IndexScanState *node);


/* ----------------------------------------------------------------
 *		IndexNext
 *
 *		Retrieve a tuple from the IndexScan node's currentRelation
 *		using the index specified in the IndexScanState information.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
IndexNext(IndexScanState *node)
{
	EState	   *estate;
	ExprContext *econtext;
	ScanDirection direction;
	IndexScanDesc scandesc;
	Index		scanrelid;
	HeapTuple	tuple;
	TupleTableSlot *slot;

	/*
	 * extract necessary information from index scan node
	 */
	estate = node->ss.ps.state;
	direction = estate->es_direction;
	/* flip direction if this is an overall backward scan */
	if (ScanDirectionIsBackward(((IndexScan *) node->ss.ps.plan)->indexorderdir))
	{
		if (ScanDirectionIsForward(direction))
			direction = BackwardScanDirection;
		else if (ScanDirectionIsBackward(direction))
			direction = ForwardScanDirection;
	}
	scandesc = node->iss_ScanDesc;
	econtext = node->ss.ps.ps_ExprContext;
	slot = node->ss.ss_ScanTupleSlot;
	scanrelid = ((IndexScan *) node->ss.ps.plan)->scan.scanrelid;

	/*
	 * Clear any reference to the previously returned tuple.  The idea here is
	 * to not have the tuple slot be the last holder of a pin on that tuple's
	 * buffer; if it is, we'll need a separate visit to the bufmgr to release
	 * the buffer.	By clearing here, we get to have the release done by
	 * ReleaseAndReadBuffer inside index_getnext.
	 */
	ExecClearTuple(slot);

	/*
	 * Check if we are evaluating PlanQual for tuple of this relation.
	 * Additional checking is not good, but no other way for now. We could
	 * introduce new nodes for this case and handle IndexScan --> NewNode
	 * switching in Init/ReScan plan...
	 */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[scanrelid - 1] != NULL)
	{
		if (estate->es_evTupleNull[scanrelid - 1])
			return slot;		/* return empty slot */

		ExecStoreTuple(estate->es_evTuple[scanrelid - 1],
					   slot, InvalidBuffer, false);

		/* Does the tuple meet the indexqual condition? */
		econtext->ecxt_scantuple = slot;

		ResetExprContext(econtext);

		if (!ExecQual(node->indexqualorig, econtext, false))
			ExecClearTuple(slot);		/* would not be returned by scan */

		/* Flag for the next call that no more tuples */
		estate->es_evTupleNull[scanrelid - 1] = true;

		return slot;
	}

	/*
	 * ok, now that we have what we need, fetch the next tuple.
	 */
	if ((tuple = index_getnext(scandesc, direction)) != NULL)
	{
		/*
		 * Store the scanned tuple in the scan tuple slot of the scan state.
		 * Note: we pass 'false' because tuples returned by amgetnext are
		 * pointers onto disk pages and must not be pfree()'d.
		 */
		ExecStoreTuple(tuple,	/* tuple to store */
					   slot,	/* slot to store in */
					   scandesc->xs_cbuf,		/* buffer containing tuple */
					   false);	/* don't pfree */

		return slot;
	}

	/*
	 * if we get here it means the index scan failed so we are at the end of
	 * the scan..
	 */
	return ExecClearTuple(slot);
}

/* ----------------------------------------------------------------
 *		ExecIndexScan(node)
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecIndexScan(IndexScanState *node)
{
	/*
	 * If we have runtime keys and they've not already been set up, do it now.
	 */
	if (node->iss_RuntimeKeyInfo && !node->iss_RuntimeKeysReady)
		ExecReScan((PlanState *) node, NULL);

	/*
	 * use IndexNext as access method
	 */
	return ExecScan(&node->ss, (ExecScanAccessMtd) IndexNext);
}

/* ----------------------------------------------------------------
 *		ExecIndexReScan(node)
 *
 *		Recalculates the value of the scan keys whose value depends on
 *		information known at runtime and rescans the indexed relation.
 *		Updating the scan key was formerly done separately in
 *		ExecUpdateIndexScanKeys. Integrating it into ReScan makes
 *		rescans of indices and relations/general streams more uniform.
 * ----------------------------------------------------------------
 */
void
ExecIndexReScan(IndexScanState *node, ExprContext *exprCtxt)
{
	EState	   *estate;
	ExprContext *econtext;
	ScanKey		scanKeys;
	ExprState **runtimeKeyInfo;
	int			numScanKeys;
	Index		scanrelid;

	estate = node->ss.ps.state;
	econtext = node->iss_RuntimeContext;		/* context for runtime keys */
	scanKeys = node->iss_ScanKeys;
	runtimeKeyInfo = node->iss_RuntimeKeyInfo;
	numScanKeys = node->iss_NumScanKeys;
	scanrelid = ((IndexScan *) node->ss.ps.plan)->scan.scanrelid;

	if (econtext)
	{
		/*
		 * If we are being passed an outer tuple, save it for runtime key
		 * calc.  We also need to link it into the "regular" per-tuple
		 * econtext, so it can be used during indexqualorig evaluations.
		 */
		if (exprCtxt != NULL)
		{
			ExprContext *stdecontext;

			econtext->ecxt_outertuple = exprCtxt->ecxt_outertuple;
			stdecontext = node->ss.ps.ps_ExprContext;
			stdecontext->ecxt_outertuple = exprCtxt->ecxt_outertuple;
		}

		/*
		 * Reset the runtime-key context so we don't leak memory as each outer
		 * tuple is scanned.  Note this assumes that we will recalculate *all*
		 * runtime keys on each call.
		 */
		ResetExprContext(econtext);
	}

	/*
	 * If we are doing runtime key calculations (ie, the index keys depend on
	 * data from an outer scan), compute the new key values
	 */
	if (runtimeKeyInfo)
	{
		ExecIndexEvalRuntimeKeys(econtext,
								 runtimeKeyInfo,
								 scanKeys,
								 numScanKeys);
		node->iss_RuntimeKeysReady = true;
	}

	/* If this is re-scanning of PlanQual ... */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[scanrelid - 1] != NULL)
	{
		estate->es_evTupleNull[scanrelid - 1] = false;
		return;
	}

	/* reset index scan */
	index_rescan(node->iss_ScanDesc, scanKeys);
}


/*
 * ExecIndexEvalRuntimeKeys
 *		Evaluate any runtime key values, and update the scankeys.
 */
void
ExecIndexEvalRuntimeKeys(ExprContext *econtext,
						 ExprState **run_keys,
						 ScanKey scan_keys,
						 int n_keys)
{
	int			j;

	for (j = 0; j < n_keys; j++)
	{
		/*
		 * If we have a run-time key, then extract the run-time expression and
		 * evaluate it with respect to the current outer tuple.  We then stick
		 * the result into the scan key.
		 *
		 * Note: the result of the eval could be a pass-by-ref value that's
		 * stored in the outer scan's tuple, not in
		 * econtext->ecxt_per_tuple_memory.  We assume that the outer tuple
		 * will stay put throughout our scan.  If this is wrong, we could copy
		 * the result into our context explicitly, but I think that's not
		 * necessary...
		 */
		if (run_keys[j] != NULL)
		{
			Datum		scanvalue;
			bool		isNull;

			scanvalue = ExecEvalExprSwitchContext(run_keys[j],
												  econtext,
												  &isNull,
												  NULL);
			scan_keys[j].sk_argument = scanvalue;
			if (isNull)
				scan_keys[j].sk_flags |= SK_ISNULL;
			else
				scan_keys[j].sk_flags &= ~SK_ISNULL;
		}
	}
}

/* ----------------------------------------------------------------
 *		ExecEndIndexScan
 * ----------------------------------------------------------------
 */
void
ExecEndIndexScan(IndexScanState *node)
{
	Relation	indexRelationDesc;
	IndexScanDesc indexScanDesc;
	Relation	relation;

	/*
	 * extract information from the node
	 */
	indexRelationDesc = node->iss_RelationDesc;
	indexScanDesc = node->iss_ScanDesc;
	relation = node->ss.ss_currentRelation;

	/*
	 * Free the exprcontext(s) ... now dead code, see ExecFreeExprContext
	 */
#ifdef NOT_USED
	ExecFreeExprContext(&node->ss.ps);
	if (node->iss_RuntimeContext)
		FreeExprContext(node->iss_RuntimeContext);
#endif

	/*
	 * clear out tuple table slots
	 */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * close the index relation
	 */
	index_endscan(indexScanDesc);
	index_close(indexRelationDesc);

	/*
	 * close the heap relation.
	 *
	 * Currently, we do not release the AccessShareLock acquired by
	 * ExecInitIndexScan.  This lock should be held till end of transaction.
	 * (There is a faction that considers this too much locking, however.)
	 */
	heap_close(relation, NoLock);
}

/* ----------------------------------------------------------------
 *		ExecIndexMarkPos
 * ----------------------------------------------------------------
 */
void
ExecIndexMarkPos(IndexScanState *node)
{
	index_markpos(node->iss_ScanDesc);
}

/* ----------------------------------------------------------------
 *		ExecIndexRestrPos
 * ----------------------------------------------------------------
 */
void
ExecIndexRestrPos(IndexScanState *node)
{
	index_restrpos(node->iss_ScanDesc);
}

/* ----------------------------------------------------------------
 *		ExecInitIndexScan
 *
 *		Initializes the index scan's state information, creates
 *		scan keys, and opens the base and index relations.
 *
 *		Note: index scans have 2 sets of state information because
 *			  we have to keep track of the base relation and the
 *			  index relation.
 * ----------------------------------------------------------------
 */
IndexScanState *
ExecInitIndexScan(IndexScan *node, EState *estate)
{
	IndexScanState *indexstate;
	ScanKey		scanKeys;
	int			numScanKeys;
	ExprState **runtimeKeyInfo;
	bool		have_runtime_keys;
	RangeTblEntry *rtentry;
	Index		relid;
	Oid			reloid;
	Relation	currentRelation;

	/*
	 * create state structure
	 */
	indexstate = makeNode(IndexScanState);
	indexstate->ss.ps.plan = (Plan *) node;
	indexstate->ss.ps.state = estate;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &indexstate->ss.ps);

	/*
	 * initialize child expressions
	 *
	 * Note: we don't initialize all of the indexqual expression, only the
	 * sub-parts corresponding to runtime keys (see below).  The indexqualorig
	 * expression is always initialized even though it will only be used in
	 * some uncommon cases --- would be nice to improve that.  (Problem is
	 * that any SubPlans present in the expression must be found now...)
	 */
	indexstate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->scan.plan.targetlist,
					 (PlanState *) indexstate);
	indexstate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->scan.plan.qual,
					 (PlanState *) indexstate);
	indexstate->indexqualorig = (List *)
		ExecInitExpr((Expr *) node->indexqualorig,
					 (PlanState *) indexstate);

#define INDEXSCAN_NSLOTS 2

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &indexstate->ss.ps);
	ExecInitScanTupleSlot(estate, &indexstate->ss);

	/*
	 * Initialize index-specific scan state
	 */
	indexstate->iss_RuntimeKeysReady = false;

	CXT1_printf("ExecInitIndexScan: context is %d\n", CurrentMemoryContext);

	/*
	 * build the index scan keys from the index qualification
	 */
	have_runtime_keys =
		ExecIndexBuildScanKeys((PlanState *) indexstate,
							   node->indexqual,
							   node->indexstrategy,
							   node->indexsubtype,
							   &runtimeKeyInfo,
							   &scanKeys,
							   &numScanKeys);

	indexstate->iss_RuntimeKeyInfo = runtimeKeyInfo;
	indexstate->iss_ScanKeys = scanKeys;
	indexstate->iss_NumScanKeys = numScanKeys;

	/*
	 * If we have runtime keys, we need an ExprContext to evaluate them. The
	 * node's standard context won't do because we want to reset that context
	 * for every tuple.  So, build another context just like the other one...
	 * -tgl 7/11/00
	 */
	if (have_runtime_keys)
	{
		ExprContext *stdecontext = indexstate->ss.ps.ps_ExprContext;

		ExecAssignExprContext(estate, &indexstate->ss.ps);
		indexstate->iss_RuntimeContext = indexstate->ss.ps.ps_ExprContext;
		indexstate->ss.ps.ps_ExprContext = stdecontext;
	}
	else
	{
		indexstate->iss_RuntimeContext = NULL;
	}

	/*
	 * open the base relation and acquire AccessShareLock on it.
	 */
	relid = node->scan.scanrelid;
	rtentry = rt_fetch(relid, estate->es_range_table);
	reloid = rtentry->relid;

	currentRelation = heap_open(reloid, AccessShareLock);

	indexstate->ss.ss_currentRelation = currentRelation;
	indexstate->ss.ss_currentScanDesc = NULL;	/* no heap scan here */

	/*
	 * get the scan type from the relation descriptor.
	 */
	ExecAssignScanType(&indexstate->ss, RelationGetDescr(currentRelation), false);

	/*
	 * open the index relation and initialize relation and scan descriptors.
	 * Note we acquire no locks here; the index machinery does its own locks
	 * and unlocks.  (We rely on having AccessShareLock on the parent table to
	 * ensure the index won't go away!)
	 */
	indexstate->iss_RelationDesc = index_open(node->indexid);
	indexstate->iss_ScanDesc = index_beginscan(currentRelation,
											   indexstate->iss_RelationDesc,
											   estate->es_snapshot,
											   numScanKeys,
											   scanKeys);

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&indexstate->ss.ps);
	ExecAssignScanProjectionInfo(&indexstate->ss);

	/*
	 * all done.
	 */
	return indexstate;
}


/*
 * ExecIndexBuildScanKeys
 *		Build the index scan keys from the index qualification
 *
 * Input params are:
 *
 * planstate: executor state node we are working for
 * quals: indexquals expressions
 * strategies: associated operator strategy numbers
 * subtypes: associated operator subtype OIDs
 *
 * Output params are:
 *
 * *runtimeKeyInfo: receives ptr to array of runtime key exprstates
 *		(NULL if no runtime keys)
 * *scanKeys: receives ptr to array of ScanKeys
 * *numScanKeys: receives number of scankeys/runtime keys
 *
 * Return value is TRUE if any runtime key expressions were found, else FALSE.
 */
bool
ExecIndexBuildScanKeys(PlanState *planstate, List *quals,
					   List *strategies, List *subtypes,
					   ExprState ***runtimeKeyInfo,
					   ScanKey *scanKeys, int *numScanKeys)
{
	bool		have_runtime_keys = false;
	ListCell   *qual_cell;
	ListCell   *strategy_cell;
	ListCell   *subtype_cell;
	int			n_keys;
	ScanKey		scan_keys;
	ExprState **run_keys;
	int			j;

	n_keys = list_length(quals);
	scan_keys = (n_keys <= 0) ? NULL :
		(ScanKey) palloc(n_keys * sizeof(ScanKeyData));
	run_keys = (n_keys <= 0) ? NULL :
		(ExprState **) palloc(n_keys * sizeof(ExprState *));

	/*
	 * for each opclause in the given qual, convert each qual's opclause into
	 * a single scan key
	 */
	qual_cell = list_head(quals);
	strategy_cell = list_head(strategies);
	subtype_cell = list_head(subtypes);

	for (j = 0; j < n_keys; j++)
	{
		OpExpr	   *clause;		/* one clause of index qual */
		Expr	   *leftop;		/* expr on lhs of operator */
		Expr	   *rightop;	/* expr on rhs ... */
		int			flags = 0;
		AttrNumber	varattno;	/* att number used in scan */
		StrategyNumber strategy;	/* op's strategy number */
		Oid			subtype;	/* op's strategy subtype */
		RegProcedure opfuncid;	/* operator proc id used in scan */
		Datum		scanvalue;	/* value used in scan (if const) */

		/*
		 * extract clause information from the qualification
		 */
		clause = (OpExpr *) lfirst(qual_cell);
		qual_cell = lnext(qual_cell);
		strategy = lfirst_int(strategy_cell);
		strategy_cell = lnext(strategy_cell);
		subtype = lfirst_oid(subtype_cell);
		subtype_cell = lnext(subtype_cell);

		if (!IsA(clause, OpExpr))
			elog(ERROR, "indexqual is not an OpExpr");

		opfuncid = clause->opfuncid;

		/*
		 * Here we figure out the contents of the index qual. The usual case
		 * is (var op const) which means we form a scan key for the attribute
		 * listed in the var node and use the value of the const as comparison
		 * data.
		 *
		 * If we don't have a const node, it means our scan key is a function
		 * of information obtained during the execution of the plan, in which
		 * case we need to recalculate the index scan key at run time.	Hence,
		 * we set have_runtime_keys to true and place the appropriate
		 * subexpression in run_keys. The corresponding scan key values are
		 * recomputed at run time.
		 */
		run_keys[j] = NULL;

		/*
		 * determine information in leftop
		 */
		leftop = (Expr *) get_leftop((Expr *) clause);

		if (leftop && IsA(leftop, RelabelType))
			leftop = ((RelabelType *) leftop)->arg;

		Assert(leftop != NULL);

		if (!(IsA(leftop, Var) &&
			  var_is_rel((Var *) leftop)))
			elog(ERROR, "indexqual doesn't have key on left side");

		varattno = ((Var *) leftop)->varattno;

		/*
		 * now determine information in rightop
		 */
		rightop = (Expr *) get_rightop((Expr *) clause);

		if (rightop && IsA(rightop, RelabelType))
			rightop = ((RelabelType *) rightop)->arg;

		Assert(rightop != NULL);

		if (IsA(rightop, Const))
		{
			/*
			 * if the rightop is a const node then it means it identifies the
			 * value to place in our scan key.
			 */
			scanvalue = ((Const *) rightop)->constvalue;
			if (((Const *) rightop)->constisnull)
				flags |= SK_ISNULL;
		}
		else
		{
			/*
			 * otherwise, the rightop contains an expression evaluable at
			 * runtime to figure out the value to place in our scan key.
			 */
			have_runtime_keys = true;
			run_keys[j] = ExecInitExpr(rightop, planstate);
			scanvalue = (Datum) 0;
		}

		/*
		 * initialize the scan key's fields appropriately
		 */
		ScanKeyEntryInitialize(&scan_keys[j],
							   flags,
							   varattno,		/* attribute number to scan */
							   strategy,		/* op's strategy */
							   subtype, /* strategy subtype */
							   opfuncid,		/* reg proc to use */
							   scanvalue);		/* constant */
	}

	/* If no runtime keys, get rid of speculatively-allocated array */
	if (run_keys && !have_runtime_keys)
	{
		pfree(run_keys);
		run_keys = NULL;
	}

	/*
	 * Return the info to our caller.
	 */
	*numScanKeys = n_keys;
	*scanKeys = scan_keys;
	*runtimeKeyInfo = run_keys;

	return have_runtime_keys;
}

int
ExecCountSlotsIndexScan(IndexScan *node)
{
	return ExecCountSlotsNode(outerPlan((Plan *) node)) +
		ExecCountSlotsNode(innerPlan((Plan *) node)) + INDEXSCAN_NSLOTS;
}
