/*-------------------------------------------------------------------------
 *
 * nodeAgg.c
 *	  Routines to handle aggregate nodes.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * NOTE
 *	  The implementation of Agg node has been reworked to handle legal
 *	  SQL aggregates. (Do not expect POSTQUEL semantics.)	 -- ay 2/95
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeAgg.c,v 1.57 1999/10/08 03:49:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_aggregate.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "optimizer/clauses.h"
#include "parser/parse_type.h"
#include "utils/syscache.h"

/*
 * AggStatePerAggData - per-aggregate working state for the Agg scan
 */
typedef struct AggStatePerAggData
{
	/*
	 * These values are set up during ExecInitAgg() and do not change
	 * thereafter:
	 */

	/* Oids of transfer functions */
	Oid			xfn1_oid;
	Oid			xfn2_oid;
	Oid			finalfn_oid;
	/*
	 * fmgr lookup data for transfer functions --- only valid when
	 * corresponding oid is not InvalidOid
	 */
	FmgrInfo	xfn1;
	FmgrInfo	xfn2;
	FmgrInfo	finalfn;
	/*
	 * initial values from pg_aggregate entry
	 */
	Datum		initValue1;		/* for transtype1 */
	Datum		initValue2;		/* for transtype2 */
	bool		initValue1IsNull,
				initValue2IsNull;
	/*
	 * We need the len and byval info for the agg's transition status types
	 * in order to know how to copy/delete values.
	 */
	int			transtype1Len,
				transtype2Len;
	bool		transtype1ByVal,
				transtype2ByVal;

	/*
	 * These values are working state that is initialized at the start
	 * of an input tuple group and updated for each input tuple:
	 */

	Datum		value1,			/* current transfer values 1 and 2 */
				value2;
	bool		value1IsNull,
				value2IsNull;
	bool		noInitValue;	/* true if value1 not set yet */
	/*
	 * Note: right now, noInitValue always has the same value as value1IsNull.
	 * But we should keep them separate because once the fmgr interface is
	 * fixed, we'll need to distinguish a null returned by transfn1 from
	 * a null we haven't yet replaced with an input value.
	 */
} AggStatePerAggData;


/*
 * Helper routine to make a copy of a Datum.
 *
 * NB: input had better not be a NULL; might cause null-pointer dereference.
 */
static Datum
copyDatum(Datum val, int typLen, bool typByVal)
{
	if (typByVal)
		return val;
	else
	{
		char   *newVal;

		if (typLen == -1)		/* variable length type? */
			typLen = VARSIZE((struct varlena *) DatumGetPointer(val));
		newVal = (char *) palloc(typLen);
		memcpy(newVal, DatumGetPointer(val), typLen);
		return PointerGetDatum(newVal);
	}
}


/* ---------------------------------------
 *
 * ExecAgg -
 *
 *	  ExecAgg receives tuples from its outer subplan and aggregates over
 *	  the appropriate attribute for each aggregate function use (Aggref
 *	  node) appearing in the targetlist or qual of the node.  The number
 *	  of tuples to aggregate over depends on whether a GROUP BY clause is
 *	  present.  We can produce an aggregate result row per group, or just
 *	  one for the whole query.  The value of each aggregate is stored in
 *	  the expression context to be used when ExecProject evaluates the
 *	  result tuple.
 *
 *	  ExecAgg evaluates each aggregate in the following steps: (initcond1,
 *	  initcond2 are the initial values and sfunc1, sfunc2, and finalfunc are
 *	  the transition functions.)
 *
 *		 value1 = initcond1
 *		 value2 = initcond2
 *		 foreach tuple do
 *			value1 = sfunc1(value1, aggregated_value)
 *			value2 = sfunc2(value2)
 *		 value1 = finalfunc(value1, value2)
 *
 *	  If initcond1 is NULL then the first non-NULL aggregated_value is
 *	  assigned directly to value1.  sfunc1 isn't applied until value1
 *	  is non-NULL.
 *
 *	  sfunc1 is never applied when the current tuple's aggregated_value
 *	  is NULL.  sfunc2 is applied for each tuple if the aggref is marked
 *	  'usenulls', otherwise it is only applied when aggregated_value is
 *	  not NULL.  (usenulls was formerly used for COUNT(*), but is no longer
 *	  needed for that purpose; as of 10/1999 the support for usenulls is
 *	  dead code.  I have not removed it because it seems like a potentially
 *	  useful feature for user-defined aggregates.  We'd just need to add a
 *	  flag column to pg_aggregate and a parameter to CREATE AGGREGATE...)
 *
 *	  If the outer subplan is a Group node, ExecAgg returns as many tuples
 *	  as there are groups.
 *
 * ------------------------------------------
 */
TupleTableSlot *
ExecAgg(Agg *node)
{
	AggState   *aggstate;
	EState	   *estate;
	Plan	   *outerPlan;
	ExprContext *econtext;
	ProjectionInfo *projInfo;
	Datum	   *aggvalues;
	bool	   *aggnulls;
	AggStatePerAgg	peragg;
	TupleTableSlot *resultSlot;
	HeapTuple	inputTuple;
	int			aggno;
	List	   *alist;
	bool		isDone;
	bool		isNull;

	/* ---------------------
	 *	get state info from node
	 * ---------------------
	 */
	aggstate = node->aggstate;
	estate = node->plan.state;
	outerPlan = outerPlan(node);
	econtext = aggstate->csstate.cstate.cs_ExprContext;
	aggvalues = econtext->ecxt_aggvalues;
	aggnulls = econtext->ecxt_aggnulls;
	projInfo = aggstate->csstate.cstate.cs_ProjInfo;
	peragg = aggstate->peragg;

	/*
	 * We loop retrieving groups until we find one matching
	 * node->plan.qual
	 */
	do
	{
		if (aggstate->agg_done)
			return NULL;

		/*
		 * Initialize working state for a new input tuple group
		 */
		aggno = -1;
		foreach(alist, aggstate->aggs)
		{
			AggStatePerAgg	peraggstate = &peragg[++aggno];

			/*
			 * (Re)set value1 and value2 to their initial values.
			 */
			if (OidIsValid(peraggstate->xfn1_oid) &&
				! peraggstate->initValue1IsNull)
				peraggstate->value1 = copyDatum(peraggstate->initValue1, 
												peraggstate->transtype1Len,
												peraggstate->transtype1ByVal);
			else
				peraggstate->value1 = (Datum) NULL;
			peraggstate->value1IsNull = peraggstate->initValue1IsNull;

			if (OidIsValid(peraggstate->xfn2_oid) &&
				! peraggstate->initValue2IsNull)
				peraggstate->value2 = copyDatum(peraggstate->initValue2, 
												peraggstate->transtype2Len,
												peraggstate->transtype2ByVal);
			else
				peraggstate->value2 = (Datum) NULL;
			peraggstate->value2IsNull = peraggstate->initValue2IsNull;

			/* ------------------------------------------
			 * If the initial value for the first transition function
			 * doesn't exist in the pg_aggregate table then we will let
			 * the first value returned from the outer procNode become
			 * the initial value. (This is useful for aggregates like
			 * max{} and min{}.)  The noInitValue flag signals that we
			 * still need to do this.
			 * ------------------------------------------
			 */
			peraggstate->noInitValue = peraggstate->initValue1IsNull;
		}

		inputTuple = NULL;		/* no saved input tuple yet */

		/* ----------------
		 *	 for each tuple from the outer plan, update all the aggregates
		 * ----------------
		 */
		for (;;)
		{
			TupleTableSlot *outerslot;

			outerslot = ExecProcNode(outerPlan, (Plan *) node);
			if (TupIsNull(outerslot))
				break;
			econtext->ecxt_scantuple = outerslot;

			aggno = -1;
			foreach(alist, aggstate->aggs)
			{
				Aggref		   *aggref = (Aggref *) lfirst(alist);
				AggStatePerAgg	peraggstate = &peragg[++aggno];
				Datum			newVal;
				Datum			args[2];

				newVal = ExecEvalExpr(aggref->target, econtext,
									  &isNull, &isDone);

				if (isNull && !aggref->usenulls)
					continue;	/* ignore this tuple for this agg */

				if (OidIsValid(peraggstate->xfn1_oid) && !isNull)
				{
					if (peraggstate->noInitValue)
					{
						/*
						 * value1 has not been initialized. This is the
						 * first non-NULL input value. We use it as the
						 * initial value for value1.  XXX We assume,
						 * without having checked, that the agg's input type
						 * is binary-compatible with its transtype1!
						 *
						 * We have to copy the datum since the tuple from
						 * which it came will be freed on the next iteration
						 * of the scan.  
						 */
						peraggstate->value1 = copyDatum(newVal,
												peraggstate->transtype1Len,
												peraggstate->transtype1ByVal);
						peraggstate->value1IsNull = false;
						peraggstate->noInitValue = false;
					}
					else
					{
						/* apply transition function 1 */
						args[0] = peraggstate->value1;
						args[1] = newVal;
						newVal = (Datum) fmgr_c(&peraggstate->xfn1,
												(FmgrValues *) args,
												&isNull);
						if (! peraggstate->transtype1ByVal)
							pfree(peraggstate->value1);
						peraggstate->value1 = newVal;
					}
				}

				if (OidIsValid(peraggstate->xfn2_oid))
				{
					/* apply transition function 2 */
					args[0] = peraggstate->value2;
					isNull = false;	/* value2 cannot be null, currently */
					newVal = (Datum) fmgr_c(&peraggstate->xfn2,
											(FmgrValues *) args,
											&isNull);
					if (! peraggstate->transtype2ByVal)
						pfree(peraggstate->value2);
					peraggstate->value2 = newVal;
				}
			}

			/*
			 * Keep a copy of the first input tuple for the projection.
			 * (We only need one since only the GROUP BY columns in it
			 * can be referenced, and these will be the same for all
			 * tuples aggregated over.)
			 */
			if (!inputTuple)
				inputTuple = heap_copytuple(outerslot->val);
		}

		/*
		 * Done scanning input tuple group.
		 * Finalize each aggregate calculation.
		 */
		aggno = -1;
		foreach(alist, aggstate->aggs)
		{
			AggStatePerAgg	peraggstate = &peragg[++aggno];
			char		   *args[2];

			/*
			 * XXX For now, only apply finalfn if we got at least one
			 * non-null input value.  This prevents zero divide in AVG().
			 * If we had cleaner handling of null inputs/results in functions,
			 * we could probably take out this hack and define the result
			 * for no inputs as whatever finalfn returns for null input.
			 */
			if (OidIsValid(peraggstate->finalfn_oid) &&
				! peraggstate->noInitValue)
			{
				if (peraggstate->finalfn.fn_nargs > 1)
				{
					args[0] = (char *) peraggstate->value1;
					args[1] = (char *) peraggstate->value2;
				}
				else if (OidIsValid(peraggstate->xfn1_oid))
					args[0] = (char *) peraggstate->value1;
				else if (OidIsValid(peraggstate->xfn2_oid))
					args[0] = (char *) peraggstate->value2;
				else
					elog(ERROR, "ExecAgg: no valid transition functions??");
				aggnulls[aggno] = false;
				aggvalues[aggno] = (Datum) fmgr_c(&peraggstate->finalfn,
												  (FmgrValues *) args,
												  &(aggnulls[aggno]));
			}
			else if (OidIsValid(peraggstate->xfn1_oid))
			{
				/* Return value1 */
				aggvalues[aggno] = peraggstate->value1;
				aggnulls[aggno] = peraggstate->value1IsNull;
				/* prevent pfree below */
				peraggstate->value1IsNull = true;
			}
			else if (OidIsValid(peraggstate->xfn2_oid))
			{
				/* Return value2 */
				aggvalues[aggno] = peraggstate->value2;
				aggnulls[aggno] = peraggstate->value2IsNull;
				/* prevent pfree below */
				peraggstate->value2IsNull = true;
			}
			else
				elog(ERROR, "ExecAgg: no valid transition functions??");

			/*
			 * Release any per-group working storage, unless we're passing
			 * it back as the result of the aggregate.
			 */
			if (OidIsValid(peraggstate->xfn1_oid) &&
				! peraggstate->value1IsNull &&
				! peraggstate->transtype1ByVal)
				pfree(peraggstate->value1);

			if (OidIsValid(peraggstate->xfn2_oid) &&
				! peraggstate->value2IsNull &&
				! peraggstate->transtype2ByVal)
				pfree(peraggstate->value2);
		}

		/*
		 * If the outerPlan is a Group node, we will reach here after each
		 * group.  We are not done unless the Group node is done (a little
		 * ugliness here while we reach into the Group's state to find out).
		 * Furthermore, when grouping we return nothing at all unless we
		 * had some input tuple(s).  By the nature of Group, there are
		 * no empty groups, so if we get here with no input the whole scan
		 * is empty.
		 *
		 * If the outerPlan isn't a Group, we are done when we get here,
		 * and we will emit a (single) tuple even if there were no input
		 * tuples.
		 */
		if (IsA(outerPlan, Group))
		{
			/* aggregation over groups */
			aggstate->agg_done = ((Group *) outerPlan)->grpstate->grp_done;
			/* check for no groups */
			if (inputTuple == NULL)
				return NULL;
		}
		else
			aggstate->agg_done = true;

		/*
		 * We used to create a dummy all-nulls input tuple here if
		 * inputTuple == NULL (ie, the outerPlan didn't return anything).
		 * However, now that we don't return a bogus tuple in Group mode,
		 * we can only get here with inputTuple == NULL in non-Group mode.
		 * So, if the parser has done its job right, the projected output
		 * tuple's targetList must not contain any direct references to
		 * input columns, and so it's a waste of time to create an
		 * all-nulls input tuple.  We just let the tuple slot get set
		 * to NULL instead.  The values returned for the aggregates will
		 * be the initial values of the transition functions.
		 */

		/*
		 * Store the representative input tuple (or NULL, if none)
		 * in the tuple table slot reserved for it.
		 */
		ExecStoreTuple(inputTuple,
					   aggstate->csstate.css_ScanTupleSlot,
					   InvalidBuffer,
					   true);
		econtext->ecxt_scantuple = aggstate->csstate.css_ScanTupleSlot;

		/*
		 * Form a projection tuple using the aggregate results and the
		 * representative input tuple.  Store it in the result tuple slot,
		 * and return it if it meets my qual condition.
		 */
		resultSlot = ExecProject(projInfo, &isDone);

		/*
		 * If the completed tuple does not match the qualifications,
		 * it is ignored and we loop back to try to process another group.
		 */
	}
	while (! ExecQual(node->plan.qual, econtext));

	return resultSlot;
}

/* -----------------
 * ExecInitAgg
 *
 *	Creates the run-time information for the agg node produced by the
 *	planner and initializes its outer subtree
 * -----------------
 */
bool
ExecInitAgg(Agg *node, EState *estate, Plan *parent)
{
	AggState	   *aggstate;
	AggStatePerAgg	peragg;
	Plan		   *outerPlan;
	ExprContext	   *econtext;
	int				numaggs,
					aggno;
	List		   *alist;

	/*
	 * assign the node's execution state
	 */
	node->plan.state = estate;

	/*
	 * create state structure
	 */
	aggstate = makeNode(AggState);
	node->aggstate = aggstate;
	aggstate->agg_done = false;

	/*
	 * find aggregates in targetlist and quals
	 */
	aggstate->aggs = nconc(pull_agg_clause((Node *) node->plan.targetlist),
						   pull_agg_clause((Node *) node->plan.qual));
	aggstate->numaggs = numaggs = length(aggstate->aggs);
	if (numaggs <= 0)
	{
		/*
		 * This used to be treated as an error, but we can't do that anymore
		 * because constant-expression simplification could optimize away
		 * all of the Aggrefs in the targetlist and qual.  So, just make a
		 * debug note, and force numaggs positive so that palloc()s below
		 * don't choke.
		 */
		elog(DEBUG, "ExecInitAgg: could not find any aggregate functions");
		numaggs = 1;
	}

	/*
	 * assign node's base id and create expression context
	 */
	ExecAssignNodeBaseInfo(estate, &aggstate->csstate.cstate, (Plan *) parent);
	ExecAssignExprContext(estate, &aggstate->csstate.cstate);

#define AGG_NSLOTS 2

	/*
	 * tuple table initialization
	 */
	ExecInitScanTupleSlot(estate, &aggstate->csstate);
	ExecInitResultTupleSlot(estate, &aggstate->csstate.cstate);

	/*
	 * Set up aggregate-result storage in the expr context,
	 * and also allocate my private per-agg working storage
	 */
	econtext = aggstate->csstate.cstate.cs_ExprContext;
	econtext->ecxt_aggvalues = (Datum *) palloc(sizeof(Datum) * numaggs);
	MemSet(econtext->ecxt_aggvalues, 0, sizeof(Datum) * numaggs);
	econtext->ecxt_aggnulls = (bool *) palloc(sizeof(bool) * numaggs);
	MemSet(econtext->ecxt_aggnulls, 0, sizeof(bool) * numaggs);

	peragg = (AggStatePerAgg) palloc(sizeof(AggStatePerAggData) * numaggs);
	MemSet(peragg, 0, sizeof(AggStatePerAggData) * numaggs);
	aggstate->peragg = peragg;

	/*
	 * initialize child nodes
	 */
	outerPlan = outerPlan(node);
	ExecInitNode(outerPlan, estate, (Plan *) node);

	/* ----------------
	 *	initialize source tuple type.
	 * ----------------
	 */
	ExecAssignScanTypeFromOuterPlan((Plan *) node, &aggstate->csstate);

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL((Plan *) node, &aggstate->csstate.cstate);
	ExecAssignProjectionInfo((Plan *) node, &aggstate->csstate.cstate);

	/*
	 * Perform lookups of aggregate function info, and initialize the
	 * unchanging fields of the per-agg data
	 */
	aggno = -1;
	foreach(alist, aggstate->aggs)
	{
		Aggref		   *aggref = (Aggref *) lfirst(alist);
		AggStatePerAgg	peraggstate = &peragg[++aggno];
		char		   *aggname = aggref->aggname;
		HeapTuple		aggTuple;
		Form_pg_aggregate aggform;
		Type			typeInfo;
		Oid				xfn1_oid,
						xfn2_oid,
						finalfn_oid;

		/* Mark Aggref node with its associated index in the result array */
		aggref->aggno = aggno;

		aggTuple = SearchSysCacheTuple(AGGNAME,
									   PointerGetDatum(aggname),
									   ObjectIdGetDatum(aggref->basetype),
									   0, 0);
		if (!HeapTupleIsValid(aggTuple))
			elog(ERROR, "ExecAgg: cache lookup failed for aggregate %s(%s)",
				 aggname,
				 typeidTypeName(aggref->basetype));
		aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);

		peraggstate->initValue1 = (Datum)
			AggNameGetInitVal(aggname,
							  aggform->aggbasetype,
							  1,
							  &peraggstate->initValue1IsNull);

		peraggstate->initValue2 = (Datum)
			AggNameGetInitVal(aggname,
							  aggform->aggbasetype,
							  2,
							  &peraggstate->initValue2IsNull);

		peraggstate->xfn1_oid = xfn1_oid = aggform->aggtransfn1;
		peraggstate->xfn2_oid = xfn2_oid = aggform->aggtransfn2;
		peraggstate->finalfn_oid = finalfn_oid = aggform->aggfinalfn;

		if (OidIsValid(xfn1_oid))
		{
			fmgr_info(xfn1_oid, &peraggstate->xfn1);
			/* If a transfn1 is specified, transtype1 had better be, too */
			typeInfo = typeidType(aggform->aggtranstype1);
			peraggstate->transtype1Len = typeLen(typeInfo);
			peraggstate->transtype1ByVal = typeByVal(typeInfo);
		}

		if (OidIsValid(xfn2_oid))
		{
			fmgr_info(xfn2_oid, &peraggstate->xfn2);
			/* If a transfn2 is specified, transtype2 had better be, too */
			typeInfo = typeidType(aggform->aggtranstype2);
			peraggstate->transtype2Len = typeLen(typeInfo);
			peraggstate->transtype2ByVal = typeByVal(typeInfo);
			/* ------------------------------------------
			 * If there is a second transition function, its initial
			 * value must exist -- as it does not depend on data values,
			 * we have no other way of determining an initial value.
			 * ------------------------------------------
			 */
			if (peraggstate->initValue2IsNull)
				elog(ERROR, "ExecInitAgg: agginitval2 is null");
		}

		if (OidIsValid(finalfn_oid))
		{
			fmgr_info(finalfn_oid, &peraggstate->finalfn);
		}
	}

	return TRUE;
}

int
ExecCountSlotsAgg(Agg *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
	ExecCountSlotsNode(innerPlan(node)) +
	AGG_NSLOTS;
}

void
ExecEndAgg(Agg *node)
{
	AggState   *aggstate = node->aggstate;
	Plan	   *outerPlan;

	ExecFreeProjectionInfo(&aggstate->csstate.cstate);

	outerPlan = outerPlan(node);
	ExecEndNode(outerPlan, (Plan *) node);

	/* clean up tuple table */
	ExecClearTuple(aggstate->csstate.css_ScanTupleSlot);
}

void
ExecReScanAgg(Agg *node, ExprContext *exprCtxt, Plan *parent)
{
	AggState   *aggstate = node->aggstate;
	ExprContext *econtext = aggstate->csstate.cstate.cs_ExprContext;

	aggstate->agg_done = false;
	MemSet(econtext->ecxt_aggvalues, 0, sizeof(Datum) * aggstate->numaggs);
	MemSet(econtext->ecxt_aggnulls, 0, sizeof(bool) * aggstate->numaggs);

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((Plan *) node)->lefttree->chgParam == NULL)
		ExecReScan(((Plan *) node)->lefttree, exprCtxt, (Plan *) node);

}
