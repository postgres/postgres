/*-------------------------------------------------------------------------
 *
 * nodeBitmapIndexscan.c
 *	  Routines to support bitmapped index scans of relations
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeBitmapIndexscan.c,v 1.1 2005/04/19 22:35:12 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		MultiExecBitmapIndexScan	scans a relation using index.
 *		ExecInitBitmapIndexScan		creates and initializes state info.
 *		ExecBitmapIndexReScan		prepares to rescan the plan.
 *		ExecEndBitmapIndexScan		releases all storage.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "executor/execdebug.h"
#include "executor/instrument.h"
#include "executor/nodeBitmapIndexscan.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "parser/parsetree.h"


/* ----------------------------------------------------------------
 *		MultiExecBitmapIndexScan(node)
 * ----------------------------------------------------------------
 */
Node *
MultiExecBitmapIndexScan(BitmapIndexScanState *node)
{
#define MAX_TIDS	1024
	TIDBitmap  *tbm;
	IndexScanDesc scandesc;
	ItemPointerData tids[MAX_TIDS];
	int32		ntids;
	double		nTuples = 0;

	/* must provide our own instrumentation support */
	if (node->ss.ps.instrument)
		InstrStartNode(node->ss.ps.instrument);

	/*
	 * If we have runtime keys and they've not already been set up, do it
	 * now.
	 */
	if (node->biss_RuntimeKeyInfo && !node->biss_RuntimeKeysReady)
		ExecReScan((PlanState *) node, NULL);

	/*
	 * extract necessary information from index scan node
	 */
	scandesc = node->biss_ScanDesc;

	/*
	 * Prepare result bitmap
	 */
	tbm = tbm_create(work_mem * 1024L);

	/*
	 * Get TIDs from index and insert into bitmap
	 */
	for (;;)
	{
		bool	more = index_getmulti(scandesc, tids, MAX_TIDS, &ntids);

		if (ntids > 0)
		{
			tbm_add_tuples(tbm, tids, ntids);
			nTuples += ntids;
		}

		if (!more)
			break;

		CHECK_FOR_INTERRUPTS();
	}

	/* must provide our own instrumentation support */
	if (node->ss.ps.instrument)
		InstrStopNodeMulti(node->ss.ps.instrument, nTuples);

	return (Node *) tbm;
}

/* ----------------------------------------------------------------
 *		ExecBitmapIndexReScan(node)
 *
 *		Recalculates the value of the scan keys whose value depends on
 *		information known at runtime and rescans the indexed relation.
 *		Updating the scan key was formerly done separately in
 *		ExecUpdateIndexScanKeys. Integrating it into ReScan makes
 *		rescans of indices and relations/general streams more uniform.
 *
 * ----------------------------------------------------------------
 */
void
ExecBitmapIndexReScan(BitmapIndexScanState *node, ExprContext *exprCtxt)
{
	ExprContext *econtext;
	ExprState **runtimeKeyInfo;
	Index		scanrelid;

	econtext = node->biss_RuntimeContext;		/* context for runtime
												 * keys */
	runtimeKeyInfo = node->biss_RuntimeKeyInfo;
	scanrelid = ((BitmapIndexScan *) node->ss.ps.plan)->scan.scanrelid;

	if (econtext)
	{
		/*
		 * If we are being passed an outer tuple, save it for runtime key
		 * calc.  We also need to link it into the "regular" per-tuple
		 * econtext.
		 */
		if (exprCtxt != NULL)
		{
			ExprContext *stdecontext;

			econtext->ecxt_outertuple = exprCtxt->ecxt_outertuple;
			stdecontext = node->ss.ps.ps_ExprContext;
			stdecontext->ecxt_outertuple = exprCtxt->ecxt_outertuple;
		}

		/*
		 * Reset the runtime-key context so we don't leak memory as each
		 * outer tuple is scanned.	Note this assumes that we will
		 * recalculate *all* runtime keys on each call.
		 */
		ResetExprContext(econtext);
	}

	/*
	 * If we are doing runtime key calculations (ie, the index keys depend
	 * on data from an outer scan), compute the new key values
	 */
	if (runtimeKeyInfo)
	{
		int			n_keys;
		ScanKey		scan_keys;
		ExprState **run_keys;
		int			j;

		n_keys = node->biss_NumScanKeys;
		scan_keys = node->biss_ScanKeys;
		run_keys = runtimeKeyInfo;

		for (j = 0; j < n_keys; j++)
		{
			/*
			 * If we have a run-time key, then extract the run-time
			 * expression and evaluate it with respect to the current
			 * outer tuple.  We then stick the result into the scan
			 * key.
			 *
			 * Note: the result of the eval could be a pass-by-ref value
			 * that's stored in the outer scan's tuple, not in
			 * econtext->ecxt_per_tuple_memory.  We assume that the
			 * outer tuple will stay put throughout our scan.  If this
			 * is wrong, we could copy the result into our context
			 * explicitly, but I think that's not necessary...
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

		node->biss_RuntimeKeysReady = true;
	}

	index_rescan(node->biss_ScanDesc, node->biss_ScanKeys);
}

/* ----------------------------------------------------------------
 *		ExecEndBitmapIndexScan
 * ----------------------------------------------------------------
 */
void
ExecEndBitmapIndexScan(BitmapIndexScanState *node)
{
	Relation	relation;

	/*
	 * extract information from the node
	 */
	relation = node->ss.ss_currentRelation;

	/*
	 * Free the exprcontext(s)
	 */
	ExecFreeExprContext(&node->ss.ps);
	if (node->biss_RuntimeContext)
		FreeExprContext(node->biss_RuntimeContext);

	/*
	 * close the index relation
	 */
	if (node->biss_ScanDesc != NULL)
		index_endscan(node->biss_ScanDesc);

	if (node->biss_RelationDesc != NULL)
		index_close(node->biss_RelationDesc);

	/*
	 * close the heap relation.
	 *
	 * Currently, we do not release the AccessShareLock acquired by
	 * ExecInitBitmapIndexScan.  This lock should be held till end of
	 * transaction. (There is a faction that considers this too much
	 * locking, however.)
	 */
	heap_close(relation, NoLock);
}

/* ----------------------------------------------------------------
 *		ExecInitBitmapIndexScan
 *
 *		Initializes the index scan's state information, creates
 *		scan keys, and opens the base and index relations.
 *
 *		Note: index scans have 2 sets of state information because
 *			  we have to keep track of the base relation and the
 *			  index relations.
 *
 * old comments
 *		Creates the run-time state information for the node and
 *		sets the relation id to contain relevant descriptors.
 *
 *		Parameters:
 *		  node: BitmapIndexNode node produced by the planner.
 *		  estate: the execution state initialized in InitPlan.
 * ----------------------------------------------------------------
 */
BitmapIndexScanState *
ExecInitBitmapIndexScan(BitmapIndexScan *node, EState *estate)
{
	BitmapIndexScanState *indexstate;
	ExprState **runtimeKeyInfo;
	bool		have_runtime_keys;
	RangeTblEntry *rtentry;
	Index		relid;
	Oid			reloid;
	Relation	currentRelation;

	/*
	 * create state structure
	 */
	indexstate = makeNode(BitmapIndexScanState);
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
	 * We don't need to initialize targetlist or qual since neither are used.
	 *
	 * Note: we don't initialize all of the indxqual expression, only the
	 * sub-parts corresponding to runtime keys (see below).
	 */

#define BITMAPINDEXSCAN_NSLOTS 0

	/*
	 * Initialize index-specific scan state
	 */
	indexstate->biss_ScanKeys = NULL;
	indexstate->biss_NumScanKeys = 0;
	indexstate->biss_RuntimeKeyInfo = NULL;
	indexstate->biss_RuntimeContext = NULL;
	indexstate->biss_RuntimeKeysReady = false;
	indexstate->biss_RelationDesc = NULL;
	indexstate->biss_ScanDesc = NULL;

	CXT1_printf("ExecInitBitmapIndexScan: context is %d\n", CurrentMemoryContext);

	/*
	 * initialize space for runtime key info (may not be needed)
	 */
	have_runtime_keys = false;

	/*
	 * build the index scan keys from the index qualification
	 */
	{
		List	   *quals;
		List	   *strategies;
		List	   *subtypes;
		ListCell   *qual_cell;
		ListCell   *strategy_cell;
		ListCell   *subtype_cell;
		int			n_keys;
		ScanKey		scan_keys;
		ExprState **run_keys;
		int			j;

		quals = node->indxqual;
		strategies = node->indxstrategy;
		subtypes = node->indxsubtype;
		n_keys = list_length(quals);
		scan_keys = (n_keys <= 0) ? NULL :
			(ScanKey) palloc(n_keys * sizeof(ScanKeyData));
		run_keys = (n_keys <= 0) ? NULL :
			(ExprState **) palloc(n_keys * sizeof(ExprState *));

		/*
		 * for each opclause in the given qual, convert each qual's
		 * opclause into a single scan key
		 */
		qual_cell = list_head(quals);
		strategy_cell = list_head(strategies);
		subtype_cell = list_head(subtypes);
		for (j = 0; j < n_keys; j++)
		{
			OpExpr	   *clause; /* one clause of index qual */
			Expr	   *leftop; /* expr on lhs of operator */
			Expr	   *rightop;	/* expr on rhs ... */
			int			flags = 0;
			AttrNumber	varattno;		/* att number used in scan */
			StrategyNumber strategy;	/* op's strategy number */
			Oid			subtype;	/* op's strategy subtype */
			RegProcedure opfuncid;		/* operator proc id used in scan */
			Datum		scanvalue;		/* value used in scan (if const) */

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
				elog(ERROR, "indxqual is not an OpExpr");

			opfuncid = clause->opfuncid;

			/*
			 * Here we figure out the contents of the index qual. The
			 * usual case is (var op const) which means we form a scan key
			 * for the attribute listed in the var node and use the value
			 * of the const as comparison data.
			 *
			 * If we don't have a const node, it means our scan key is a
			 * function of information obtained during the execution of
			 * the plan, in which case we need to recalculate the index
			 * scan key at run time.  Hence, we set have_runtime_keys to
			 * true and place the appropriate subexpression in run_keys.
			 * The corresponding scan key values are recomputed at run
			 * time.
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
				elog(ERROR, "indxqual doesn't have key on left side");

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
				 * if the rightop is a const node then it means it
				 * identifies the value to place in our scan key.
				 */
				scanvalue = ((Const *) rightop)->constvalue;
				if (((Const *) rightop)->constisnull)
					flags |= SK_ISNULL;
			}
			else
			{
				/*
				 * otherwise, the rightop contains an expression evaluable
				 * at runtime to figure out the value to place in our scan
				 * key.
				 */
				have_runtime_keys = true;
				run_keys[j] = ExecInitExpr(rightop, (PlanState *) indexstate);
				scanvalue = (Datum) 0;
			}

			/*
			 * initialize the scan key's fields appropriately
			 */
			ScanKeyEntryInitialize(&scan_keys[j],
								   flags,
								   varattno,	/* attribute number to
												 * scan */
								   strategy,	/* op's strategy */
								   subtype,		/* strategy subtype */
								   opfuncid,	/* reg proc to use */
								   scanvalue);	/* constant */
		}

		/*
		 * store the key information into the node.
		 */
		indexstate->biss_NumScanKeys = n_keys;
		indexstate->biss_ScanKeys = scan_keys;
		runtimeKeyInfo = run_keys;
	}


	/*
	 * If all of our keys have the form (var op const), then we have no
	 * runtime keys so we store NULL in the runtime key info. Otherwise
	 * runtime key info contains an array of pointers (one for each index)
	 * to arrays of flags (one for each key) which indicate that the qual
	 * needs to be evaluated at runtime. -cim 10/24/89
	 *
	 * If we do have runtime keys, we need an ExprContext to evaluate them;
	 * the node's standard context won't do because we want to reset that
	 * context for every tuple.  So, build another context just like the
	 * other one... -tgl 7/11/00
	 */
	if (have_runtime_keys)
	{
		ExprContext *stdecontext = indexstate->ss.ps.ps_ExprContext;

		ExecAssignExprContext(estate, &indexstate->ss.ps);
		indexstate->biss_RuntimeKeyInfo = runtimeKeyInfo;
		indexstate->biss_RuntimeContext = indexstate->ss.ps.ps_ExprContext;
		indexstate->ss.ps.ps_ExprContext = stdecontext;
	}
	else
	{
		indexstate->biss_RuntimeKeyInfo = NULL;
		indexstate->biss_RuntimeContext = NULL;
		/* Get rid of the speculatively-allocated flag array, too */
		pfree(runtimeKeyInfo);
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
	 * open the index relation and initialize relation and scan
	 * descriptors.  Note we acquire no locks here; the index machinery
	 * does its own locks and unlocks.	(We rely on having AccessShareLock
	 * on the parent table to ensure the index won't go away!)
	 */
	indexstate->biss_RelationDesc = index_open(node->indxid);
	indexstate->biss_ScanDesc =
		index_beginscan_multi(indexstate->biss_RelationDesc,
							  estate->es_snapshot,
							  indexstate->biss_NumScanKeys,
							  indexstate->biss_ScanKeys);

	/*
	 * all done.
	 */
	return indexstate;
}

int
ExecCountSlotsBitmapIndexScan(BitmapIndexScan *node)
{
	return ExecCountSlotsNode(outerPlan((Plan *) node)) +
		ExecCountSlotsNode(innerPlan((Plan *) node)) + BITMAPINDEXSCAN_NSLOTS;
}
