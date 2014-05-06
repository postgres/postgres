/*-------------------------------------------------------------------------
 *
 * nodeIndexscan.c
 *	  Routines to support indexed scans of relations
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeIndexscan.c,v 1.132.2.2 2009/08/23 18:26:15 tgl Exp $
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
#include "access/nbtree.h"
#include "access/relscan.h"
#include "executor/execdebug.h"
#include "executor/nodeIndexscan.h"
#include "optimizer/clauses.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
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
	 * Check if we are evaluating PlanQual for tuple of this relation.
	 * Additional checking is not good, but no other way for now. We could
	 * introduce new nodes for this case and handle IndexScan --> NewNode
	 * switching in Init/ReScan plan...
	 */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[scanrelid - 1] != NULL)
	{
		if (estate->es_evTupleNull[scanrelid - 1])
			return ExecClearTuple(slot);

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
	while ((tuple = index_getnext(scandesc, direction)) != NULL)
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

		/*
		 * If the index was lossy, we have to recheck the index quals using
		 * the real tuple.
		 */
		if (scandesc->xs_recheck)
		{
			econtext->ecxt_scantuple = slot;
			ResetExprContext(econtext);
			if (!ExecQual(node->indexqualorig, econtext, false))
				continue;		/* nope, so ask index for another one */
		}

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
	if (node->iss_NumRuntimeKeys != 0 && !node->iss_RuntimeKeysReady)
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
	Index		scanrelid;

	estate = node->ss.ps.state;
	econtext = node->iss_RuntimeContext;		/* context for runtime keys */
	scanrelid = ((IndexScan *) node->ss.ps.plan)->scan.scanrelid;

	node->ss.ps.ps_TupFromTlist = false;

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
	if (node->iss_NumRuntimeKeys != 0)
		ExecIndexEvalRuntimeKeys(econtext,
								 node->iss_RuntimeKeys,
								 node->iss_NumRuntimeKeys);
	node->iss_RuntimeKeysReady = true;

	/* If this is re-scanning of PlanQual ... */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[scanrelid - 1] != NULL)
	{
		estate->es_evTupleNull[scanrelid - 1] = false;
		return;
	}

	/* reset index scan */
	index_rescan(node->iss_ScanDesc, node->iss_ScanKeys);
}


/*
 * ExecIndexEvalRuntimeKeys
 *		Evaluate any runtime key values, and update the scankeys.
 */
void
ExecIndexEvalRuntimeKeys(ExprContext *econtext,
						 IndexRuntimeKeyInfo *runtimeKeys, int numRuntimeKeys)
{
	int			j;
	MemoryContext oldContext;

	/* We want to keep the key values in per-tuple memory */
	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	for (j = 0; j < numRuntimeKeys; j++)
	{
		ScanKey		scan_key = runtimeKeys[j].scan_key;
		ExprState  *key_expr = runtimeKeys[j].key_expr;
		Datum		scanvalue;
		bool		isNull;

		/*
		 * For each run-time key, extract the run-time expression and evaluate
		 * it with respect to the current outer tuple.  We then stick the
		 * result into the proper scan key.
		 *
		 * Note: the result of the eval could be a pass-by-ref value that's
		 * stored in the outer scan's tuple, not in
		 * econtext->ecxt_per_tuple_memory.  We assume that the outer tuple
		 * will stay put throughout our scan.  If this is wrong, we could copy
		 * the result into our context explicitly, but I think that's not
		 * necessary.
		 *
		 * It's also entirely possible that the result of the eval is a
		 * toasted value.  In this case we should forcibly detoast it,
		 * to avoid repeat detoastings each time the value is examined
		 * by an index support function.
		 */
		scanvalue = ExecEvalExpr(key_expr,
								 econtext,
								 &isNull,
								 NULL);
		if (isNull)
		{
			scan_key->sk_argument = scanvalue;
			scan_key->sk_flags |= SK_ISNULL;
		}
		else
		{
			if (runtimeKeys[j].key_toastable)
				scanvalue = PointerGetDatum(PG_DETOAST_DATUM(scanvalue));
			scan_key->sk_argument = scanvalue;
			scan_key->sk_flags &= ~SK_ISNULL;
		}
	}

	MemoryContextSwitchTo(oldContext);
}

/*
 * ExecIndexEvalArrayKeys
 *		Evaluate any array key values, and set up to iterate through arrays.
 *
 * Returns TRUE if there are array elements to consider; FALSE means there
 * is at least one null or empty array, so no match is possible.  On TRUE
 * result, the scankeys are initialized with the first elements of the arrays.
 */
bool
ExecIndexEvalArrayKeys(ExprContext *econtext,
					   IndexArrayKeyInfo *arrayKeys, int numArrayKeys)
{
	bool		result = true;
	int			j;
	MemoryContext oldContext;

	/* We want to keep the arrays in per-tuple memory */
	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	for (j = 0; j < numArrayKeys; j++)
	{
		ScanKey		scan_key = arrayKeys[j].scan_key;
		ExprState  *array_expr = arrayKeys[j].array_expr;
		Datum		arraydatum;
		bool		isNull;
		ArrayType  *arrayval;
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		int			num_elems;
		Datum	   *elem_values;
		bool	   *elem_nulls;

		/*
		 * Compute and deconstruct the array expression. (Notes in
		 * ExecIndexEvalRuntimeKeys() apply here too.)
		 */
		arraydatum = ExecEvalExpr(array_expr,
								  econtext,
								  &isNull,
								  NULL);
		if (isNull)
		{
			result = false;
			break;				/* no point in evaluating more */
		}
		arrayval = DatumGetArrayTypeP(arraydatum);
		/* We could cache this data, but not clear it's worth it */
		get_typlenbyvalalign(ARR_ELEMTYPE(arrayval),
							 &elmlen, &elmbyval, &elmalign);
		deconstruct_array(arrayval,
						  ARR_ELEMTYPE(arrayval),
						  elmlen, elmbyval, elmalign,
						  &elem_values, &elem_nulls, &num_elems);
		if (num_elems <= 0)
		{
			result = false;
			break;				/* no point in evaluating more */
		}

		/*
		 * Note: we expect the previous array data, if any, to be
		 * automatically freed by resetting the per-tuple context; hence no
		 * pfree's here.
		 */
		arrayKeys[j].elem_values = elem_values;
		arrayKeys[j].elem_nulls = elem_nulls;
		arrayKeys[j].num_elems = num_elems;
		scan_key->sk_argument = elem_values[0];
		if (elem_nulls[0])
			scan_key->sk_flags |= SK_ISNULL;
		else
			scan_key->sk_flags &= ~SK_ISNULL;
		arrayKeys[j].next_elem = 1;
	}

	MemoryContextSwitchTo(oldContext);

	return result;
}

/*
 * ExecIndexAdvanceArrayKeys
 *		Advance to the next set of array key values, if any.
 *
 * Returns TRUE if there is another set of values to consider, FALSE if not.
 * On TRUE result, the scankeys are initialized with the next set of values.
 */
bool
ExecIndexAdvanceArrayKeys(IndexArrayKeyInfo *arrayKeys, int numArrayKeys)
{
	bool		found = false;
	int			j;

	/*
	 * Note we advance the rightmost array key most quickly, since it will
	 * correspond to the lowest-order index column among the available
	 * qualifications.  This is hypothesized to result in better locality of
	 * access in the index.
	 */
	for (j = numArrayKeys - 1; j >= 0; j--)
	{
		ScanKey		scan_key = arrayKeys[j].scan_key;
		int			next_elem = arrayKeys[j].next_elem;
		int			num_elems = arrayKeys[j].num_elems;
		Datum	   *elem_values = arrayKeys[j].elem_values;
		bool	   *elem_nulls = arrayKeys[j].elem_nulls;

		if (next_elem >= num_elems)
		{
			next_elem = 0;
			found = false;		/* need to advance next array key */
		}
		else
			found = true;
		scan_key->sk_argument = elem_values[next_elem];
		if (elem_nulls[next_elem])
			scan_key->sk_flags |= SK_ISNULL;
		else
			scan_key->sk_flags &= ~SK_ISNULL;
		arrayKeys[j].next_elem = next_elem + 1;
		if (found)
			break;
	}

	return found;
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
		FreeExprContext(node->iss_RuntimeContext, true);
#endif

	/*
	 * clear out tuple table slots
	 */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * close the index relation (no-op if we didn't open it)
	 */
	if (indexScanDesc)
		index_endscan(indexScanDesc);
	if (indexRelationDesc)
		index_close(indexRelationDesc, NoLock);

	/*
	 * close the heap relation.
	 */
	ExecCloseScanRelation(relation);
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
ExecInitIndexScan(IndexScan *node, EState *estate, int eflags)
{
	IndexScanState *indexstate;
	Relation	currentRelation;
	bool		relistarget;

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

	indexstate->ss.ps.ps_TupFromTlist = false;

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
	 * open the base relation and acquire appropriate lock on it.
	 */
	currentRelation = ExecOpenScanRelation(estate, node->scan.scanrelid);

	indexstate->ss.ss_currentRelation = currentRelation;
	indexstate->ss.ss_currentScanDesc = NULL;	/* no heap scan here */

	/*
	 * get the scan type from the relation descriptor.
	 */
	ExecAssignScanType(&indexstate->ss, RelationGetDescr(currentRelation));

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&indexstate->ss.ps);
	ExecAssignScanProjectionInfo(&indexstate->ss);

	/*
	 * If we are just doing EXPLAIN (ie, aren't going to run the plan), stop
	 * here.  This allows an index-advisor plugin to EXPLAIN a plan containing
	 * references to nonexistent indexes.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return indexstate;

	/*
	 * Open the index relation.
	 *
	 * If the parent table is one of the target relations of the query, then
	 * InitPlan already opened and write-locked the index, so we can avoid
	 * taking another lock here.  Otherwise we need a normal reader's lock.
	 */
	relistarget = ExecRelationIsTargetRelation(estate, node->scan.scanrelid);
	indexstate->iss_RelationDesc = index_open(node->indexid,
									 relistarget ? NoLock : AccessShareLock);

	/*
	 * Initialize index-specific scan state
	 */
	indexstate->iss_RuntimeKeysReady = false;

	/*
	 * build the index scan keys from the index qualification
	 */
	ExecIndexBuildScanKeys((PlanState *) indexstate,
						   indexstate->iss_RelationDesc,
						   node->scan.scanrelid,
						   node->indexqual,
						   &indexstate->iss_ScanKeys,
						   &indexstate->iss_NumScanKeys,
						   &indexstate->iss_RuntimeKeys,
						   &indexstate->iss_NumRuntimeKeys,
						   NULL,	/* no ArrayKeys */
						   NULL);

	/*
	 * If we have runtime keys, we need an ExprContext to evaluate them. The
	 * node's standard context won't do because we want to reset that context
	 * for every tuple.  So, build another context just like the other one...
	 * -tgl 7/11/00
	 */
	if (indexstate->iss_NumRuntimeKeys != 0)
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
	 * Initialize scan descriptor.
	 */
	indexstate->iss_ScanDesc = index_beginscan(currentRelation,
											   indexstate->iss_RelationDesc,
											   estate->es_snapshot,
											   indexstate->iss_NumScanKeys,
											   indexstate->iss_ScanKeys);

	/*
	 * all done.
	 */
	return indexstate;
}


/*
 * ExecIndexBuildScanKeys
 *		Build the index scan keys from the index qualification expressions
 *
 * The index quals are passed to the index AM in the form of a ScanKey array.
 * This routine sets up the ScanKeys, fills in all constant fields of the
 * ScanKeys, and prepares information about the keys that have non-constant
 * comparison values.  We divide index qual expressions into five types:
 *
 * 1. Simple operator with constant comparison value ("indexkey op constant").
 * For these, we just fill in a ScanKey containing the constant value.
 *
 * 2. Simple operator with non-constant value ("indexkey op expression").
 * For these, we create a ScanKey with everything filled in except the
 * expression value, and set up an IndexRuntimeKeyInfo struct to drive
 * evaluation of the expression at the right times.
 *
 * 3. RowCompareExpr ("(indexkey, indexkey, ...) op (expr, expr, ...)").
 * For these, we create a header ScanKey plus a subsidiary ScanKey array,
 * as specified in access/skey.h.  The elements of the row comparison
 * can have either constant or non-constant comparison values.
 *
 * 4. ScalarArrayOpExpr ("indexkey op ANY (array-expression)").  For these,
 * we create a ScanKey with everything filled in except the comparison value,
 * and set up an IndexArrayKeyInfo struct to drive processing of the qual.
 * (Note that we treat all array-expressions as requiring runtime evaluation,
 * even if they happen to be constants.)
 *
 * 5. NullTest ("indexkey IS NULL").  We just fill in the ScanKey properly.
 *
 * Input params are:
 *
 * planstate: executor state node we are working for
 * index: the index we are building scan keys for
 * scanrelid: varno of the index's relation within current query
 * quals: indexquals expressions
 *
 * Output params are:
 *
 * *scanKeys: receives ptr to array of ScanKeys
 * *numScanKeys: receives number of scankeys
 * *runtimeKeys: receives ptr to array of IndexRuntimeKeyInfos, or NULL if none
 * *numRuntimeKeys: receives number of runtime keys
 * *arrayKeys: receives ptr to array of IndexArrayKeyInfos, or NULL if none
 * *numArrayKeys: receives number of array keys
 *
 * Caller may pass NULL for arrayKeys and numArrayKeys to indicate that
 * ScalarArrayOpExpr quals are not supported.
 */
void
ExecIndexBuildScanKeys(PlanState *planstate, Relation index, Index scanrelid,
					   List *quals, ScanKey *scanKeys, int *numScanKeys,
					   IndexRuntimeKeyInfo **runtimeKeys, int *numRuntimeKeys,
					   IndexArrayKeyInfo **arrayKeys, int *numArrayKeys)
{
	ListCell   *qual_cell;
	ScanKey		scan_keys;
	IndexRuntimeKeyInfo *runtime_keys;
	IndexArrayKeyInfo *array_keys;
	int			n_scan_keys;
	int			extra_scan_keys;
	int			n_runtime_keys;
	int			n_array_keys;
	int			j;

	/*
	 * If there are any RowCompareExpr quals, we need extra ScanKey entries
	 * for them, and possibly extra runtime-key entries.  Count up what's
	 * needed.  (The subsidiary ScanKey arrays for the RowCompareExprs could
	 * be allocated as separate chunks, but we have to count anyway to make
	 * runtime_keys large enough, so might as well just do one palloc.)
	 */
	n_scan_keys = list_length(quals);
	extra_scan_keys = 0;
	foreach(qual_cell, quals)
	{
		if (IsA(lfirst(qual_cell), RowCompareExpr))
			extra_scan_keys +=
				list_length(((RowCompareExpr *) lfirst(qual_cell))->opnos);
	}
	scan_keys = (ScanKey)
		palloc((n_scan_keys + extra_scan_keys) * sizeof(ScanKeyData));
	/* Allocate these arrays as large as they could possibly need to be */
	runtime_keys = (IndexRuntimeKeyInfo *)
		palloc((n_scan_keys + extra_scan_keys) * sizeof(IndexRuntimeKeyInfo));
	array_keys = (IndexArrayKeyInfo *)
		palloc0(n_scan_keys * sizeof(IndexArrayKeyInfo));
	n_runtime_keys = 0;
	n_array_keys = 0;

	/*
	 * Below here, extra_scan_keys is index of first cell to use for next
	 * RowCompareExpr
	 */
	extra_scan_keys = n_scan_keys;

	/*
	 * for each opclause in the given qual, convert the opclause into a single
	 * scan key
	 */
	j = 0;
	foreach(qual_cell, quals)
	{
		Expr	   *clause = (Expr *) lfirst(qual_cell);
		ScanKey		this_scan_key = &scan_keys[j++];
		Oid			opno;		/* operator's OID */
		RegProcedure opfuncid;	/* operator proc id used in scan */
		Oid			opfamily;	/* opfamily of index column */
		int			op_strategy;	/* operator's strategy number */
		Oid			op_lefttype;	/* operator's declared input types */
		Oid			op_righttype;
		Expr	   *leftop;		/* expr on lhs of operator */
		Expr	   *rightop;	/* expr on rhs ... */
		AttrNumber	varattno;	/* att number used in scan */

		if (IsA(clause, OpExpr))
		{
			/* indexkey op const or indexkey op expression */
			int			flags = 0;
			Datum		scanvalue;

			opno = ((OpExpr *) clause)->opno;
			opfuncid = ((OpExpr *) clause)->opfuncid;

			/*
			 * leftop should be the index key Var, possibly relabeled
			 */
			leftop = (Expr *) get_leftop(clause);

			if (leftop && IsA(leftop, RelabelType))
				leftop = ((RelabelType *) leftop)->arg;

			Assert(leftop != NULL);

			if (!(IsA(leftop, Var) &&
				  ((Var *) leftop)->varno == scanrelid))
				elog(ERROR, "indexqual doesn't have key on left side");

			varattno = ((Var *) leftop)->varattno;
			if (varattno < 1 || varattno > index->rd_index->indnatts)
				elog(ERROR, "bogus index qualification");

			/*
			 * We have to look up the operator's strategy number.  This
			 * provides a cross-check that the operator does match the index.
			 */
			opfamily = index->rd_opfamily[varattno - 1];

			get_op_opfamily_properties(opno, opfamily,
									   &op_strategy,
									   &op_lefttype,
									   &op_righttype);

			/*
			 * rightop is the constant or variable comparison value
			 */
			rightop = (Expr *) get_rightop(clause);

			if (rightop && IsA(rightop, RelabelType))
				rightop = ((RelabelType *) rightop)->arg;

			Assert(rightop != NULL);

			if (IsA(rightop, Const))
			{
				/* OK, simple constant comparison value */
				scanvalue = ((Const *) rightop)->constvalue;
				if (((Const *) rightop)->constisnull)
					flags |= SK_ISNULL;
			}
			else
			{
				/* Need to treat this one as a runtime key */
				runtime_keys[n_runtime_keys].scan_key = this_scan_key;
				runtime_keys[n_runtime_keys].key_expr =
					ExecInitExpr(rightop, planstate);
				runtime_keys[n_runtime_keys].key_toastable =
					TypeIsToastable(op_righttype);
				n_runtime_keys++;
				scanvalue = (Datum) 0;
			}

			/*
			 * initialize the scan key's fields appropriately
			 */
			ScanKeyEntryInitialize(this_scan_key,
								   flags,
								   varattno,	/* attribute number to scan */
								   op_strategy, /* op's strategy */
								   op_righttype,		/* strategy subtype */
								   opfuncid,	/* reg proc to use */
								   scanvalue);	/* constant */
		}
		else if (IsA(clause, RowCompareExpr))
		{
			/* (indexkey, indexkey, ...) op (expression, expression, ...) */
			RowCompareExpr *rc = (RowCompareExpr *) clause;
			ListCell   *largs_cell = list_head(rc->largs);
			ListCell   *rargs_cell = list_head(rc->rargs);
			ListCell   *opnos_cell = list_head(rc->opnos);
			ScanKey		first_sub_key = &scan_keys[extra_scan_keys];

			/* Scan RowCompare columns and generate subsidiary ScanKey items */
			while (opnos_cell != NULL)
			{
				ScanKey		this_sub_key = &scan_keys[extra_scan_keys];
				int			flags = SK_ROW_MEMBER;
				Datum		scanvalue;

				/*
				 * leftop should be the index key Var, possibly relabeled
				 */
				leftop = (Expr *) lfirst(largs_cell);
				largs_cell = lnext(largs_cell);

				if (leftop && IsA(leftop, RelabelType))
					leftop = ((RelabelType *) leftop)->arg;

				Assert(leftop != NULL);

				if (!(IsA(leftop, Var) &&
					  ((Var *) leftop)->varno == scanrelid))
					elog(ERROR, "indexqual doesn't have key on left side");

				varattno = ((Var *) leftop)->varattno;

				/*
				 * We have to look up the operator's associated btree support
				 * function
				 */
				opno = lfirst_oid(opnos_cell);
				opnos_cell = lnext(opnos_cell);

				if (index->rd_rel->relam != BTREE_AM_OID ||
					varattno < 1 || varattno > index->rd_index->indnatts)
					elog(ERROR, "bogus RowCompare index qualification");
				opfamily = index->rd_opfamily[varattno - 1];

				get_op_opfamily_properties(opno, opfamily,
										   &op_strategy,
										   &op_lefttype,
										   &op_righttype);

				if (op_strategy != rc->rctype)
					elog(ERROR, "RowCompare index qualification contains wrong operator");

				opfuncid = get_opfamily_proc(opfamily,
											 op_lefttype,
											 op_righttype,
											 BTORDER_PROC);

				/*
				 * rightop is the constant or variable comparison value
				 */
				rightop = (Expr *) lfirst(rargs_cell);
				rargs_cell = lnext(rargs_cell);

				if (rightop && IsA(rightop, RelabelType))
					rightop = ((RelabelType *) rightop)->arg;

				Assert(rightop != NULL);

				if (IsA(rightop, Const))
				{
					/* OK, simple constant comparison value */
					scanvalue = ((Const *) rightop)->constvalue;
					if (((Const *) rightop)->constisnull)
						flags |= SK_ISNULL;
				}
				else
				{
					/* Need to treat this one as a runtime key */
					runtime_keys[n_runtime_keys].scan_key = this_sub_key;
					runtime_keys[n_runtime_keys].key_expr =
						ExecInitExpr(rightop, planstate);
					runtime_keys[n_runtime_keys].key_toastable =
						TypeIsToastable(op_righttype);
					n_runtime_keys++;
					scanvalue = (Datum) 0;
				}

				/*
				 * initialize the subsidiary scan key's fields appropriately
				 */
				ScanKeyEntryInitialize(this_sub_key,
									   flags,
									   varattno,		/* attribute number */
									   op_strategy,		/* op's strategy */
									   op_righttype,	/* strategy subtype */
									   opfuncid,		/* reg proc to use */
									   scanvalue);		/* constant */
				extra_scan_keys++;
			}

			/* Mark the last subsidiary scankey correctly */
			scan_keys[extra_scan_keys - 1].sk_flags |= SK_ROW_END;

			/*
			 * We don't use ScanKeyEntryInitialize for the header because it
			 * isn't going to contain a valid sk_func pointer.
			 */
			MemSet(this_scan_key, 0, sizeof(ScanKeyData));
			this_scan_key->sk_flags = SK_ROW_HEADER;
			this_scan_key->sk_attno = first_sub_key->sk_attno;
			this_scan_key->sk_strategy = rc->rctype;
			/* sk_subtype, sk_func not used in a header */
			this_scan_key->sk_argument = PointerGetDatum(first_sub_key);
		}
		else if (IsA(clause, ScalarArrayOpExpr))
		{
			/* indexkey op ANY (array-expression) */
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;

			Assert(saop->useOr);
			opno = saop->opno;
			opfuncid = saop->opfuncid;

			/*
			 * leftop should be the index key Var, possibly relabeled
			 */
			leftop = (Expr *) linitial(saop->args);

			if (leftop && IsA(leftop, RelabelType))
				leftop = ((RelabelType *) leftop)->arg;

			Assert(leftop != NULL);

			if (!(IsA(leftop, Var) &&
				  ((Var *) leftop)->varno == scanrelid))
				elog(ERROR, "indexqual doesn't have key on left side");

			varattno = ((Var *) leftop)->varattno;
			if (varattno < 1 || varattno > index->rd_index->indnatts)
				elog(ERROR, "bogus index qualification");

			/*
			 * We have to look up the operator's strategy number.  This
			 * provides a cross-check that the operator does match the index.
			 */
			opfamily = index->rd_opfamily[varattno - 1];

			get_op_opfamily_properties(opno, opfamily,
									   &op_strategy,
									   &op_lefttype,
									   &op_righttype);

			/*
			 * rightop is the constant or variable array value
			 */
			rightop = (Expr *) lsecond(saop->args);

			if (rightop && IsA(rightop, RelabelType))
				rightop = ((RelabelType *) rightop)->arg;

			Assert(rightop != NULL);

			array_keys[n_array_keys].scan_key = this_scan_key;
			array_keys[n_array_keys].array_expr =
				ExecInitExpr(rightop, planstate);
			/* the remaining fields were zeroed by palloc0 */
			n_array_keys++;

			/*
			 * initialize the scan key's fields appropriately
			 */
			ScanKeyEntryInitialize(this_scan_key,
								   0,	/* flags */
								   varattno,	/* attribute number to scan */
								   op_strategy, /* op's strategy */
								   op_righttype,		/* strategy subtype */
								   opfuncid,	/* reg proc to use */
								   (Datum) 0);	/* constant */
		}
		else if (IsA(clause, NullTest))
		{
			/* indexkey IS NULL */
			Assert(((NullTest *) clause)->nulltesttype == IS_NULL);

			/*
			 * argument should be the index key Var, possibly relabeled
			 */
			leftop = ((NullTest *) clause)->arg;

			if (leftop && IsA(leftop, RelabelType))
				leftop = ((RelabelType *) leftop)->arg;

			Assert(leftop != NULL);

			if (!(IsA(leftop, Var) &&
				  ((Var *) leftop)->varno == scanrelid))
				elog(ERROR, "NullTest indexqual has wrong key");

			varattno = ((Var *) leftop)->varattno;

			/*
			 * initialize the scan key's fields appropriately
			 */
			ScanKeyEntryInitialize(this_scan_key,
								   SK_ISNULL | SK_SEARCHNULL,
								   varattno,	/* attribute number to scan */
								   InvalidStrategy,		/* no strategy */
								   InvalidOid,	/* no strategy subtype */
								   InvalidOid,	/* no reg proc for this */
								   (Datum) 0);	/* constant */
		}
		else
			elog(ERROR, "unsupported indexqual type: %d",
				 (int) nodeTag(clause));
	}

	/* Get rid of any unused arrays */
	if (n_runtime_keys == 0)
	{
		pfree(runtime_keys);
		runtime_keys = NULL;
	}
	if (n_array_keys == 0)
	{
		pfree(array_keys);
		array_keys = NULL;
	}

	/*
	 * Return info to our caller.
	 */
	*scanKeys = scan_keys;
	*numScanKeys = n_scan_keys;
	*runtimeKeys = runtime_keys;
	*numRuntimeKeys = n_runtime_keys;
	if (arrayKeys)
	{
		*arrayKeys = array_keys;
		*numArrayKeys = n_array_keys;
	}
	else if (n_array_keys != 0)
		elog(ERROR, "ScalarArrayOpExpr index qual found where not allowed");
}

int
ExecCountSlotsIndexScan(IndexScan *node)
{
	return ExecCountSlotsNode(outerPlan((Plan *) node)) +
		ExecCountSlotsNode(innerPlan((Plan *) node)) + INDEXSCAN_NSLOTS;
}
