/*-------------------------------------------------------------------------
 *
 * nodeSubplan.c
 *	  routines to support subselects
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeSubplan.c,v 1.58 2003/10/01 21:30:52 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		ExecSubPlan  - process a subselect
 *		ExecInitSubPlan - initialize a subselect
 *		ExecEndSubPlan	- shut down a subselect
 */
#include "postgres.h"

#include "access/heapam.h"
#include "executor/executor.h"
#include "executor/nodeSubplan.h"
#include "nodes/makefuncs.h"
#include "parser/parse_expr.h"
#include "utils/array.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"


static Datum ExecHashSubPlan(SubPlanState *node,
				ExprContext *econtext,
				bool *isNull);
static Datum ExecScanSubPlan(SubPlanState *node,
				ExprContext *econtext,
				bool *isNull);
static void buildSubPlanHash(SubPlanState *node);
static bool findPartialMatch(TupleHashTable hashtable, TupleTableSlot *slot);
static bool tupleAllNulls(HeapTuple tuple);


/* ----------------------------------------------------------------
 *		ExecSubPlan
 * ----------------------------------------------------------------
 */
Datum
ExecSubPlan(SubPlanState *node,
			ExprContext *econtext,
			bool *isNull)
{
	SubPlan    *subplan = (SubPlan *) node->xprstate.expr;

	if (subplan->setParam != NIL)
		elog(ERROR, "cannot set parent params from subquery");

	if (subplan->useHashTable)
		return ExecHashSubPlan(node, econtext, isNull);
	else
		return ExecScanSubPlan(node, econtext, isNull);
}

/*
 * ExecHashSubPlan: store subselect result in an in-memory hash table
 */
static Datum
ExecHashSubPlan(SubPlanState *node,
				ExprContext *econtext,
				bool *isNull)
{
	SubPlan    *subplan = (SubPlan *) node->xprstate.expr;
	PlanState  *planstate = node->planstate;
	ExprContext *innerecontext = node->innerecontext;
	TupleTableSlot *slot;
	HeapTuple	tup;

	/* Shouldn't have any direct correlation Vars */
	if (subplan->parParam != NIL || node->args != NIL)
		elog(ERROR, "hashed subplan with direct correlation not supported");

	/*
	 * If first time through or we need to rescan the subplan, build the
	 * hash table.
	 */
	if (node->hashtable == NULL || planstate->chgParam != NULL)
		buildSubPlanHash(node);

	/*
	 * The result for an empty subplan is always FALSE; no need to
	 * evaluate lefthand side.
	 */
	*isNull = false;
	if (!node->havehashrows && !node->havenullrows)
		return BoolGetDatum(false);

	/*
	 * Evaluate lefthand expressions and form a projection tuple. First we
	 * have to set the econtext to use (hack alert!).
	 */
	node->projLeft->pi_exprContext = econtext;
	slot = ExecProject(node->projLeft, NULL);
	tup = slot->val;

	/*
	 * Note: because we are typically called in a per-tuple context, we
	 * have to explicitly clear the projected tuple before returning.
	 * Otherwise, we'll have a double-free situation: the per-tuple
	 * context will probably be reset before we're called again, and then
	 * the tuple slot will think it still needs to free the tuple.
	 */

	/*
	 * Since the hashtable routines will use innerecontext's per-tuple
	 * memory as working memory, be sure to reset it for each tuple.
	 */
	ResetExprContext(innerecontext);

	/*
	 * If the LHS is all non-null, probe for an exact match in the main
	 * hash table.	If we find one, the result is TRUE. Otherwise, scan
	 * the partly-null table to see if there are any rows that aren't
	 * provably unequal to the LHS; if so, the result is UNKNOWN.  (We
	 * skip that part if we don't care about UNKNOWN.) Otherwise, the
	 * result is FALSE.
	 *
	 * Note: the reason we can avoid a full scan of the main hash table is
	 * that the combining operators are assumed never to yield NULL when
	 * both inputs are non-null.  If they were to do so, we might need to
	 * produce UNKNOWN instead of FALSE because of an UNKNOWN result in
	 * comparing the LHS to some main-table entry --- which is a
	 * comparison we will not even make, unless there's a chance match of
	 * hash keys.
	 */
	if (HeapTupleNoNulls(tup))
	{
		if (node->havehashrows &&
			LookupTupleHashEntry(node->hashtable, slot, NULL) != NULL)
		{
			ExecClearTuple(slot);
			return BoolGetDatum(true);
		}
		if (node->havenullrows &&
			findPartialMatch(node->hashnulls, slot))
		{
			ExecClearTuple(slot);
			*isNull = true;
			return BoolGetDatum(false);
		}
		ExecClearTuple(slot);
		return BoolGetDatum(false);
	}

	/*
	 * When the LHS is partly or wholly NULL, we can never return TRUE. If
	 * we don't care about UNKNOWN, just return FALSE.  Otherwise, if the
	 * LHS is wholly NULL, immediately return UNKNOWN.	(Since the
	 * combining operators are strict, the result could only be FALSE if
	 * the sub-select were empty, but we already handled that case.)
	 * Otherwise, we must scan both the main and partly-null tables to see
	 * if there are any rows that aren't provably unequal to the LHS; if
	 * so, the result is UNKNOWN.  Otherwise, the result is FALSE.
	 */
	if (node->hashnulls == NULL)
	{
		ExecClearTuple(slot);
		return BoolGetDatum(false);
	}
	if (tupleAllNulls(tup))
	{
		ExecClearTuple(slot);
		*isNull = true;
		return BoolGetDatum(false);
	}
	/* Scan partly-null table first, since more likely to get a match */
	if (node->havenullrows &&
		findPartialMatch(node->hashnulls, slot))
	{
		ExecClearTuple(slot);
		*isNull = true;
		return BoolGetDatum(false);
	}
	if (node->havehashrows &&
		findPartialMatch(node->hashtable, slot))
	{
		ExecClearTuple(slot);
		*isNull = true;
		return BoolGetDatum(false);
	}
	ExecClearTuple(slot);
	return BoolGetDatum(false);
}

/*
 * ExecScanSubPlan: default case where we have to rescan subplan each time
 */
static Datum
ExecScanSubPlan(SubPlanState *node,
				ExprContext *econtext,
				bool *isNull)
{
	SubPlan    *subplan = (SubPlan *) node->xprstate.expr;
	PlanState  *planstate = node->planstate;
	SubLinkType subLinkType = subplan->subLinkType;
	bool		useOr = subplan->useOr;
	MemoryContext oldcontext;
	TupleTableSlot *slot;
	Datum		result;
	bool		found = false;	/* TRUE if got at least one subplan tuple */
	List	   *pvar;
	List	   *lst;
	ArrayBuildState *astate = NULL;

	/*
	 * We are probably in a short-lived expression-evaluation context.
	 * Switch to the child plan's per-query context for manipulating its
	 * chgParam, calling ExecProcNode on it, etc.
	 */
	oldcontext = MemoryContextSwitchTo(node->sub_estate->es_query_cxt);

	/*
	 * Set Params of this plan from parent plan correlation values. (Any
	 * calculation we have to do is done in the parent econtext, since the
	 * Param values don't need to have per-query lifetime.)
	 */
	pvar = node->args;
	foreach(lst, subplan->parParam)
	{
		int			paramid = lfirsti(lst);
		ParamExecData *prm = &(econtext->ecxt_param_exec_vals[paramid]);

		Assert(pvar != NIL);
		prm->value = ExecEvalExprSwitchContext((ExprState *) lfirst(pvar),
											   econtext,
											   &(prm->isnull),
											   NULL);
		pvar = lnext(pvar);
		planstate->chgParam = bms_add_member(planstate->chgParam, paramid);
	}
	Assert(pvar == NIL);

	ExecReScan(planstate, NULL);

	/*
	 * For all sublink types except EXPR_SUBLINK and ARRAY_SUBLINK, the
	 * result is boolean as are the results of the combining operators. We
	 * combine results within a tuple (if there are multiple columns)
	 * using OR semantics if "useOr" is true, AND semantics if not. We
	 * then combine results across tuples (if the subplan produces more
	 * than one) using OR semantics for ANY_SUBLINK or AND semantics for
	 * ALL_SUBLINK. (MULTIEXPR_SUBLINK doesn't allow multiple tuples from
	 * the subplan.) NULL results from the combining operators are handled
	 * according to the usual SQL semantics for OR and AND.  The result
	 * for no input tuples is FALSE for ANY_SUBLINK, TRUE for ALL_SUBLINK,
	 * NULL for MULTIEXPR_SUBLINK.
	 *
	 * For EXPR_SUBLINK we require the subplan to produce no more than one
	 * tuple, else an error is raised. For ARRAY_SUBLINK we allow the
	 * subplan to produce more than one tuple. In either case, if zero
	 * tuples are produced, we return NULL. Assuming we get a tuple, we
	 * just use its first column (there can be only one non-junk column in
	 * this case).
	 */
	result = BoolGetDatum(subLinkType == ALL_SUBLINK);
	*isNull = false;

	for (slot = ExecProcNode(planstate);
		 !TupIsNull(slot);
		 slot = ExecProcNode(planstate))
	{
		HeapTuple	tup = slot->val;
		TupleDesc	tdesc = slot->ttc_tupleDescriptor;
		Datum		rowresult = BoolGetDatum(!useOr);
		bool		rownull = false;
		int			col = 1;
		List	   *plst;

		if (subLinkType == EXISTS_SUBLINK)
		{
			found = true;
			result = BoolGetDatum(true);
			break;
		}

		if (subLinkType == EXPR_SUBLINK)
		{
			/* cannot allow multiple input tuples for EXPR sublink */
			if (found)
				ereport(ERROR,
						(errcode(ERRCODE_CARDINALITY_VIOLATION),
						 errmsg("more than one row returned by a subquery used as an expression")));
			found = true;

			/*
			 * We need to copy the subplan's tuple in case the result is
			 * of pass-by-ref type --- our return value will point into
			 * this copied tuple!  Can't use the subplan's instance of the
			 * tuple since it won't still be valid after next
			 * ExecProcNode() call. node->curTuple keeps track of the
			 * copied tuple for eventual freeing.
			 */
			MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
			tup = heap_copytuple(tup);
			if (node->curTuple)
				heap_freetuple(node->curTuple);
			node->curTuple = tup;
			MemoryContextSwitchTo(node->sub_estate->es_query_cxt);

			result = heap_getattr(tup, col, tdesc, isNull);
			/* keep scanning subplan to make sure there's only one tuple */
			continue;
		}

		if (subLinkType == ARRAY_SUBLINK)
		{
			Datum		dvalue;
			bool		disnull;

			found = true;
			/* stash away current value */
			dvalue = heap_getattr(tup, 1, tdesc, &disnull);
			astate = accumArrayResult(astate, dvalue, disnull,
									  tdesc->attrs[0]->atttypid,
									  oldcontext);
			/* keep scanning subplan to collect all values */
			continue;
		}

		/* cannot allow multiple input tuples for MULTIEXPR sublink either */
		if (subLinkType == MULTIEXPR_SUBLINK && found)
			ereport(ERROR,
					(errcode(ERRCODE_CARDINALITY_VIOLATION),
					 errmsg("more than one row returned by a subquery used as an expression")));

		found = true;

		/*
		 * For ALL, ANY, and MULTIEXPR sublinks, iterate over combining
		 * operators for columns of tuple.
		 */
		plst = subplan->paramIds;
		foreach(lst, node->exprs)
		{
			ExprState  *exprstate = (ExprState *) lfirst(lst);
			int			paramid = lfirsti(plst);
			ParamExecData *prmdata;
			Datum		expresult;
			bool		expnull;

			/*
			 * Load up the Param representing this column of the
			 * sub-select.
			 */
			prmdata = &(econtext->ecxt_param_exec_vals[paramid]);
			Assert(prmdata->execPlan == NULL);
			prmdata->value = heap_getattr(tup, col, tdesc,
										  &(prmdata->isnull));

			/*
			 * Now we can eval the combining operator for this column.
			 */
			expresult = ExecEvalExprSwitchContext(exprstate, econtext,
												  &expnull, NULL);

			/*
			 * Combine the result into the row result as appropriate.
			 */
			if (col == 1)
			{
				rowresult = expresult;
				rownull = expnull;
			}
			else if (useOr)
			{
				/* combine within row per OR semantics */
				if (expnull)
					rownull = true;
				else if (DatumGetBool(expresult))
				{
					rowresult = BoolGetDatum(true);
					rownull = false;
					break;		/* needn't look at any more columns */
				}
			}
			else
			{
				/* combine within row per AND semantics */
				if (expnull)
					rownull = true;
				else if (!DatumGetBool(expresult))
				{
					rowresult = BoolGetDatum(false);
					rownull = false;
					break;		/* needn't look at any more columns */
				}
			}

			plst = lnext(plst);
			col++;
		}

		if (subLinkType == ANY_SUBLINK)
		{
			/* combine across rows per OR semantics */
			if (rownull)
				*isNull = true;
			else if (DatumGetBool(rowresult))
			{
				result = BoolGetDatum(true);
				*isNull = false;
				break;			/* needn't look at any more rows */
			}
		}
		else if (subLinkType == ALL_SUBLINK)
		{
			/* combine across rows per AND semantics */
			if (rownull)
				*isNull = true;
			else if (!DatumGetBool(rowresult))
			{
				result = BoolGetDatum(false);
				*isNull = false;
				break;			/* needn't look at any more rows */
			}
		}
		else
		{
			/* must be MULTIEXPR_SUBLINK */
			result = rowresult;
			*isNull = rownull;
		}
	}

	if (!found)
	{
		/*
		 * deal with empty subplan result.	result/isNull were previously
		 * initialized correctly for all sublink types except EXPR, ARRAY,
		 * and MULTIEXPR; for those, return NULL.
		 */
		if (subLinkType == EXPR_SUBLINK ||
			subLinkType == ARRAY_SUBLINK ||
			subLinkType == MULTIEXPR_SUBLINK)
		{
			result = (Datum) 0;
			*isNull = true;
		}
	}
	else if (subLinkType == ARRAY_SUBLINK)
	{
		Assert(astate != NULL);
		/* We return the result in the caller's context */
		result = makeArrayResult(astate, oldcontext);
	}

	MemoryContextSwitchTo(oldcontext);

	return result;
}

/*
 * buildSubPlanHash: load hash table by scanning subplan output.
 */
static void
buildSubPlanHash(SubPlanState *node)
{
	SubPlan    *subplan = (SubPlan *) node->xprstate.expr;
	PlanState  *planstate = node->planstate;
	int			ncols = length(node->exprs);
	ExprContext *innerecontext = node->innerecontext;
	MemoryContext tempcxt = innerecontext->ecxt_per_tuple_memory;
	MemoryContext oldcontext;
	int			nbuckets;
	TupleTableSlot *slot;

	Assert(subplan->subLinkType == ANY_SUBLINK);
	Assert(!subplan->useOr);

	/*
	 * If we already had any hash tables, destroy 'em; then create empty
	 * hash table(s).
	 *
	 * If we need to distinguish accurately between FALSE and UNKNOWN (i.e.,
	 * NULL) results of the IN operation, then we have to store subplan
	 * output rows that are partly or wholly NULL.	We store such rows in
	 * a separate hash table that we expect will be much smaller than the
	 * main table.	(We can use hashing to eliminate partly-null rows that
	 * are not distinct.  We keep them separate to minimize the cost of
	 * the inevitable full-table searches; see findPartialMatch.)
	 *
	 * If it's not necessary to distinguish FALSE and UNKNOWN, then we don't
	 * need to store subplan output rows that contain NULL.
	 */
	MemoryContextReset(node->tablecxt);
	node->hashtable = NULL;
	node->hashnulls = NULL;
	node->havehashrows = false;
	node->havenullrows = false;

	nbuckets = (int) ceil(planstate->plan->plan_rows);
	if (nbuckets < 1)
		nbuckets = 1;

	node->hashtable = BuildTupleHashTable(ncols,
										  node->keyColIdx,
										  node->eqfunctions,
										  node->hashfunctions,
										  nbuckets,
										  sizeof(TupleHashEntryData),
										  node->tablecxt,
										  tempcxt);

	if (!subplan->unknownEqFalse)
	{
		if (ncols == 1)
			nbuckets = 1;		/* there can only be one entry */
		else
		{
			nbuckets /= 16;
			if (nbuckets < 1)
				nbuckets = 1;
		}
		node->hashnulls = BuildTupleHashTable(ncols,
											  node->keyColIdx,
											  node->eqfunctions,
											  node->hashfunctions,
											  nbuckets,
											  sizeof(TupleHashEntryData),
											  node->tablecxt,
											  tempcxt);
	}

	/*
	 * We are probably in a short-lived expression-evaluation context.
	 * Switch to the child plan's per-query context for calling
	 * ExecProcNode.
	 */
	oldcontext = MemoryContextSwitchTo(node->sub_estate->es_query_cxt);

	/*
	 * Reset subplan to start.
	 */
	ExecReScan(planstate, NULL);

	/*
	 * Scan the subplan and load the hash table(s).  Note that when there
	 * are duplicate rows coming out of the sub-select, only one copy is
	 * stored.
	 */
	for (slot = ExecProcNode(planstate);
		 !TupIsNull(slot);
		 slot = ExecProcNode(planstate))
	{
		HeapTuple	tup = slot->val;
		TupleDesc	tdesc = slot->ttc_tupleDescriptor;
		int			col = 1;
		List	   *plst;
		bool		isnew;

		/*
		 * Load up the Params representing the raw sub-select outputs,
		 * then form the projection tuple to store in the hashtable.
		 */
		foreach(plst, subplan->paramIds)
		{
			int			paramid = lfirsti(plst);
			ParamExecData *prmdata;

			prmdata = &(innerecontext->ecxt_param_exec_vals[paramid]);
			Assert(prmdata->execPlan == NULL);
			prmdata->value = heap_getattr(tup, col, tdesc,
										  &(prmdata->isnull));
			col++;
		}
		slot = ExecProject(node->projRight, NULL);
		tup = slot->val;

		/*
		 * If result contains any nulls, store separately or not at all.
		 * (Since we know the projection tuple has no junk columns, we can
		 * just look at the overall hasnull info bit, instead of groveling
		 * through the columns.)
		 */
		if (HeapTupleNoNulls(tup))
		{
			(void) LookupTupleHashEntry(node->hashtable, slot, &isnew);
			node->havehashrows = true;
		}
		else if (node->hashnulls)
		{
			(void) LookupTupleHashEntry(node->hashnulls, slot, &isnew);
			node->havenullrows = true;
		}

		/*
		 * Reset innerecontext after each inner tuple to free any memory
		 * used in hash computation or comparison routines.
		 */
		ResetExprContext(innerecontext);
	}

	/*
	 * Since the projected tuples are in the sub-query's context and not
	 * the main context, we'd better clear the tuple slot before there's
	 * any chance of a reset of the sub-query's context.  Else we will
	 * have the potential for a double free attempt.
	 */
	ExecClearTuple(node->projRight->pi_slot);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * findPartialMatch: does the hashtable contain an entry that is not
 * provably distinct from the tuple?
 *
 * We have to scan the whole hashtable; we can't usefully use hashkeys
 * to guide probing, since we might get partial matches on tuples with
 * hashkeys quite unrelated to what we'd get from the given tuple.
 */
static bool
findPartialMatch(TupleHashTable hashtable, TupleTableSlot *slot)
{
	int			numCols = hashtable->numCols;
	AttrNumber *keyColIdx = hashtable->keyColIdx;
	HeapTuple	tuple = slot->val;
	TupleDesc	tupdesc = slot->ttc_tupleDescriptor;
	TupleHashIterator hashiter;
	TupleHashEntry entry;

	ResetTupleHashIterator(hashtable, &hashiter);
	while ((entry = ScanTupleHashTable(&hashiter)) != NULL)
	{
		if (!execTuplesUnequal(entry->firstTuple,
							   tuple,
							   tupdesc,
							   numCols, keyColIdx,
							   hashtable->eqfunctions,
							   hashtable->tempcxt))
			return true;
	}
	return false;
}

/*
 * tupleAllNulls: is the tuple completely NULL?
 */
static bool
tupleAllNulls(HeapTuple tuple)
{
	int			ncols = tuple->t_data->t_natts;
	int			i;

	for (i = 1; i <= ncols; i++)
	{
		if (!heap_attisnull(tuple, i))
			return false;
	}
	return true;
}

/* ----------------------------------------------------------------
 *		ExecInitSubPlan
 * ----------------------------------------------------------------
 */
void
ExecInitSubPlan(SubPlanState *node, EState *estate)
{
	SubPlan    *subplan = (SubPlan *) node->xprstate.expr;
	EState	   *sp_estate;
	MemoryContext oldcontext;

	/*
	 * Do access checking on the rangetable entries in the subquery. Here,
	 * we assume the subquery is a SELECT.
	 */
	ExecCheckRTPerms(subplan->rtable, CMD_SELECT);

	/*
	 * initialize my state
	 */
	node->needShutdown = false;
	node->curTuple = NULL;
	node->projLeft = NULL;
	node->projRight = NULL;
	node->hashtable = NULL;
	node->hashnulls = NULL;
	node->tablecxt = NULL;
	node->innerecontext = NULL;
	node->keyColIdx = NULL;
	node->eqfunctions = NULL;
	node->hashfunctions = NULL;

	/*
	 * create an EState for the subplan
	 *
	 * The subquery needs its own EState because it has its own rangetable.
	 * It shares our Param ID space, however.  XXX if rangetable access
	 * were done differently, the subquery could share our EState, which
	 * would eliminate some thrashing about in this module...
	 */
	sp_estate = CreateExecutorState();
	node->sub_estate = sp_estate;

	oldcontext = MemoryContextSwitchTo(sp_estate->es_query_cxt);

	sp_estate->es_range_table = subplan->rtable;
	sp_estate->es_param_list_info = estate->es_param_list_info;
	sp_estate->es_param_exec_vals = estate->es_param_exec_vals;
	sp_estate->es_tupleTable =
		ExecCreateTupleTable(ExecCountSlotsNode(subplan->plan) + 10);
	sp_estate->es_snapshot = estate->es_snapshot;
	sp_estate->es_crosscheck_snapshot = estate->es_crosscheck_snapshot;
	sp_estate->es_instrument = estate->es_instrument;

	/*
	 * Start up the subplan (this is a very cut-down form of InitPlan())
	 */
	node->planstate = ExecInitNode(subplan->plan, sp_estate);

	node->needShutdown = true;	/* now we need to shutdown the subplan */

	MemoryContextSwitchTo(oldcontext);

	/*
	 * If this plan is un-correlated or undirect correlated one and want
	 * to set params for parent plan then mark parameters as needing
	 * evaluation.
	 *
	 * Note that in the case of un-correlated subqueries we don't care about
	 * setting parent->chgParam here: indices take care about it, for
	 * others - it doesn't matter...
	 */
	if (subplan->setParam != NIL)
	{
		List	   *lst;

		foreach(lst, subplan->setParam)
		{
			int			paramid = lfirsti(lst);
			ParamExecData *prm = &(estate->es_param_exec_vals[paramid]);

			prm->execPlan = node;
		}
	}

	/*
	 * If we are going to hash the subquery output, initialize relevant
	 * stuff.  (We don't create the hashtable until needed, though.)
	 */
	if (subplan->useHashTable)
	{
		int			ncols,
					i;
		TupleDesc	tupDesc;
		TupleTable	tupTable;
		TupleTableSlot *slot;
		List	   *lefttlist,
				   *righttlist,
				   *leftptlist,
				   *rightptlist,
				   *lexpr;

		/* We need a memory context to hold the hash table(s) */
		node->tablecxt =
			AllocSetContextCreate(CurrentMemoryContext,
								  "Subplan HashTable Context",
								  ALLOCSET_DEFAULT_MINSIZE,
								  ALLOCSET_DEFAULT_INITSIZE,
								  ALLOCSET_DEFAULT_MAXSIZE);
		/* and a short-lived exprcontext for function evaluation */
		node->innerecontext = CreateExprContext(estate);
		/* Silly little array of column numbers 1..n */
		ncols = length(node->exprs);
		node->keyColIdx = (AttrNumber *) palloc(ncols * sizeof(AttrNumber));
		for (i = 0; i < ncols; i++)
			node->keyColIdx[i] = i + 1;

		/*
		 * We use ExecProject to evaluate the lefthand and righthand
		 * expression lists and form tuples.  (You might think that we
		 * could use the sub-select's output tuples directly, but that is
		 * not the case if we had to insert any run-time coercions of the
		 * sub-select's output datatypes; anyway this avoids storing any
		 * resjunk columns that might be in the sub-select's output.) Run
		 * through the combining expressions to build tlists for the
		 * lefthand and righthand sides.  We need both the ExprState list
		 * (for ExecProject) and the underlying parse Exprs (for
		 * ExecTypeFromTL).
		 *
		 * We also extract the combining operators themselves to initialize
		 * the equality and hashing functions for the hash tables.
		 */
		lefttlist = righttlist = NIL;
		leftptlist = rightptlist = NIL;
		node->eqfunctions = (FmgrInfo *) palloc(ncols * sizeof(FmgrInfo));
		node->hashfunctions = (FmgrInfo *) palloc(ncols * sizeof(FmgrInfo));
		i = 1;
		foreach(lexpr, node->exprs)
		{
			FuncExprState *fstate = (FuncExprState *) lfirst(lexpr);
			OpExpr	   *opexpr = (OpExpr *) fstate->xprstate.expr;
			ExprState  *exstate;
			Expr	   *expr;
			TargetEntry *tle;
			GenericExprState *tlestate;
			Oid			hashfn;

			Assert(IsA(fstate, FuncExprState));
			Assert(IsA(opexpr, OpExpr));
			Assert(length(fstate->args) == 2);

			/* Process lefthand argument */
			exstate = (ExprState *) lfirst(fstate->args);
			expr = exstate->expr;
			tle = makeTargetEntry(makeResdom(i,
											 exprType((Node *) expr),
											 exprTypmod((Node *) expr),
											 NULL,
											 false),
								  expr);
			tlestate = makeNode(GenericExprState);
			tlestate->xprstate.expr = (Expr *) tle;
			tlestate->arg = exstate;
			lefttlist = lappend(lefttlist, tlestate);
			leftptlist = lappend(leftptlist, tle);

			/* Process righthand argument */
			exstate = (ExprState *) lsecond(fstate->args);
			expr = exstate->expr;
			tle = makeTargetEntry(makeResdom(i,
											 exprType((Node *) expr),
											 exprTypmod((Node *) expr),
											 NULL,
											 false),
								  expr);
			tlestate = makeNode(GenericExprState);
			tlestate->xprstate.expr = (Expr *) tle;
			tlestate->arg = exstate;
			righttlist = lappend(righttlist, tlestate);
			rightptlist = lappend(rightptlist, tle);

			/* Lookup the combining function */
			fmgr_info(opexpr->opfuncid, &node->eqfunctions[i - 1]);
			node->eqfunctions[i - 1].fn_expr = (Node *) opexpr;

			/* Lookup the associated hash function */
			hashfn = get_op_hash_function(opexpr->opno);
			if (!OidIsValid(hashfn))
				elog(ERROR, "could not find hash function for hash operator %u",
					 opexpr->opno);
			fmgr_info(hashfn, &node->hashfunctions[i - 1]);

			i++;
		}

		/*
		 * Create a tupletable to hold these tuples.  (Note: we never
		 * bother to free the tupletable explicitly; that's okay because
		 * it will never store raw disk tuples that might have associated
		 * buffer pins.  The only resource involved is memory, which will
		 * be cleaned up by freeing the query context.)
		 */
		tupTable = ExecCreateTupleTable(2);

		/*
		 * Construct tupdescs, slots and projection nodes for left and
		 * right sides.  The lefthand expressions will be evaluated in the
		 * parent plan node's exprcontext, which we don't have access to
		 * here.  Fortunately we can just pass NULL for now and fill it in
		 * later (hack alert!).  The righthand expressions will be
		 * evaluated in our own innerecontext.
		 */
		tupDesc = ExecTypeFromTL(leftptlist, false);
		slot = ExecAllocTableSlot(tupTable);
		ExecSetSlotDescriptor(slot, tupDesc, true);
		node->projLeft = ExecBuildProjectionInfo(lefttlist,
												 NULL,
												 slot);

		tupDesc = ExecTypeFromTL(rightptlist, false);
		slot = ExecAllocTableSlot(tupTable);
		ExecSetSlotDescriptor(slot, tupDesc, true);
		node->projRight = ExecBuildProjectionInfo(righttlist,
												  node->innerecontext,
												  slot);
	}
}

/* ----------------------------------------------------------------
 *		ExecSetParamPlan
 *
 *		Executes an InitPlan subplan and sets its output parameters.
 *
 * This is called from ExecEvalParam() when the value of a PARAM_EXEC
 * parameter is requested and the param's execPlan field is set (indicating
 * that the param has not yet been evaluated).	This allows lazy evaluation
 * of initplans: we don't run the subplan until/unless we need its output.
 * Note that this routine MUST clear the execPlan fields of the plan's
 * output parameters after evaluating them!
 * ----------------------------------------------------------------
 */
void
ExecSetParamPlan(SubPlanState *node, ExprContext *econtext)
{
	SubPlan    *subplan = (SubPlan *) node->xprstate.expr;
	PlanState  *planstate = node->planstate;
	SubLinkType subLinkType = subplan->subLinkType;
	MemoryContext oldcontext;
	TupleTableSlot *slot;
	List	   *lst;
	bool		found = false;
	ArrayBuildState *astate = NULL;

	/*
	 * Must switch to child query's per-query memory context.
	 */
	oldcontext = MemoryContextSwitchTo(node->sub_estate->es_query_cxt);

	if (subLinkType == ANY_SUBLINK ||
		subLinkType == ALL_SUBLINK)
		elog(ERROR, "ANY/ALL subselect unsupported as initplan");

	if (planstate->chgParam != NULL)
		ExecReScan(planstate, NULL);

	for (slot = ExecProcNode(planstate);
		 !TupIsNull(slot);
		 slot = ExecProcNode(planstate))
	{
		HeapTuple	tup = slot->val;
		TupleDesc	tdesc = slot->ttc_tupleDescriptor;
		int			i = 1;

		if (subLinkType == EXISTS_SUBLINK)
		{
			/* There can be only one param... */
			int			paramid = lfirsti(subplan->setParam);
			ParamExecData *prm = &(econtext->ecxt_param_exec_vals[paramid]);

			prm->execPlan = NULL;
			prm->value = BoolGetDatum(true);
			prm->isnull = false;
			found = true;
			break;
		}

		if (subLinkType == ARRAY_SUBLINK)
		{
			Datum		dvalue;
			bool		disnull;

			found = true;
			/* stash away current value */
			dvalue = heap_getattr(tup, 1, tdesc, &disnull);
			astate = accumArrayResult(astate, dvalue, disnull,
									  tdesc->attrs[0]->atttypid,
									  oldcontext);
			/* keep scanning subplan to collect all values */
			continue;
		}

		if (found &&
			(subLinkType == EXPR_SUBLINK ||
			 subLinkType == MULTIEXPR_SUBLINK))
			ereport(ERROR,
					(errcode(ERRCODE_CARDINALITY_VIOLATION),
					 errmsg("more than one row returned by a subquery used as an expression")));

		found = true;

		/*
		 * We need to copy the subplan's tuple into our own context, in
		 * case any of the params are pass-by-ref type --- the pointers
		 * stored in the param structs will point at this copied tuple!
		 * node->curTuple keeps track of the copied tuple for eventual
		 * freeing.
		 */
		MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
		tup = heap_copytuple(tup);
		if (node->curTuple)
			heap_freetuple(node->curTuple);
		node->curTuple = tup;
		MemoryContextSwitchTo(node->sub_estate->es_query_cxt);

		/*
		 * Now set all the setParam params from the columns of the tuple
		 */
		foreach(lst, subplan->setParam)
		{
			int			paramid = lfirsti(lst);
			ParamExecData *prm = &(econtext->ecxt_param_exec_vals[paramid]);

			prm->execPlan = NULL;
			prm->value = heap_getattr(tup, i, tdesc, &(prm->isnull));
			i++;
		}
	}

	if (!found)
	{
		if (subLinkType == EXISTS_SUBLINK)
		{
			/* There can be only one param... */
			int			paramid = lfirsti(subplan->setParam);
			ParamExecData *prm = &(econtext->ecxt_param_exec_vals[paramid]);

			prm->execPlan = NULL;
			prm->value = BoolGetDatum(false);
			prm->isnull = false;
		}
		else
		{
			foreach(lst, subplan->setParam)
			{
				int			paramid = lfirsti(lst);
				ParamExecData *prm = &(econtext->ecxt_param_exec_vals[paramid]);

				prm->execPlan = NULL;
				prm->value = (Datum) 0;
				prm->isnull = true;
			}
		}
	}
	else if (subLinkType == ARRAY_SUBLINK)
	{
		/* There can be only one param... */
		int			paramid = lfirsti(subplan->setParam);
		ParamExecData *prm = &(econtext->ecxt_param_exec_vals[paramid]);

		Assert(astate != NULL);
		prm->execPlan = NULL;
		/* We build the result in query context so it won't disappear */
		prm->value = makeArrayResult(astate, econtext->ecxt_per_query_memory);
		prm->isnull = false;
	}

	MemoryContextSwitchTo(oldcontext);
}

/* ----------------------------------------------------------------
 *		ExecEndSubPlan
 * ----------------------------------------------------------------
 */
void
ExecEndSubPlan(SubPlanState *node)
{
	if (node->needShutdown)
	{
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(node->sub_estate->es_query_cxt);
		ExecEndPlan(node->planstate, node->sub_estate);
		MemoryContextSwitchTo(oldcontext);
		FreeExecutorState(node->sub_estate);
		node->sub_estate = NULL;
		node->planstate = NULL;
		node->needShutdown = false;
	}
}

/*
 * Mark an initplan as needing recalculation
 */
void
ExecReScanSetParamPlan(SubPlanState *node, PlanState *parent)
{
	PlanState  *planstate = node->planstate;
	SubPlan    *subplan = (SubPlan *) node->xprstate.expr;
	EState	   *estate = parent->state;
	List	   *lst;

	/* sanity checks */
	if (subplan->parParam != NIL)
		elog(ERROR, "direct correlated subquery unsupported as initplan");
	if (subplan->setParam == NIL)
		elog(ERROR, "setParam list of initplan is empty");
	if (bms_is_empty(planstate->plan->extParam))
		elog(ERROR, "extParam set of initplan is empty");

	/*
	 * Don't actually re-scan: ExecSetParamPlan does it if needed.
	 */

	/*
	 * Mark this subplan's output parameters as needing recalculation
	 */
	foreach(lst, subplan->setParam)
	{
		int			paramid = lfirsti(lst);
		ParamExecData *prm = &(estate->es_param_exec_vals[paramid]);

		prm->execPlan = node;
		parent->chgParam = bms_add_member(parent->chgParam, paramid);
	}
}
