/*-------------------------------------------------------------------------
 *
 * nodeAgg.c
 *	  Routines to handle aggregate nodes.
 *
 *	  ExecAgg evaluates each aggregate in the following steps:
 *
 *		 transvalue = initcond
 *		 foreach input_value do
 *			transvalue = transfunc(transvalue, input_value)
 *		 result = finalfunc(transvalue)
 *
 *	  If a finalfunc is not supplied then the result is just the ending
 *	  value of transvalue.
 *
 *	  If transfunc is marked "strict" in pg_proc and initcond is NULL,
 *	  then the first non-NULL input_value is assigned directly to transvalue,
 *	  and transfunc isn't applied until the second non-NULL input_value.
 *	  The agg's input type and transtype must be the same in this case!
 *
 *	  If transfunc is marked "strict" then NULL input_values are skipped,
 *	  keeping the previous transvalue.	If transfunc is not strict then it
 *	  is called for every input tuple and must deal with NULL initcond
 *	  or NULL input_value for itself.
 *
 *	  If finalfunc is marked "strict" then it is not called when the
 *	  ending transvalue is NULL, instead a NULL result is created
 *	  automatically (this is just the usual handling of strict functions,
 *	  of course).  A non-strict finalfunc can make its own choice of
 *	  what to return for a NULL ending transvalue.
 *
 *	  We compute aggregate input expressions and run the transition functions
 *	  in a temporary econtext (aggstate->tmpcontext).  This is reset at
 *	  least once per input tuple, so when the transvalue datatype is
 *	  pass-by-reference, we have to be careful to copy it into a longer-lived
 *	  memory context, and free the prior value to avoid memory leakage.
 *	  We store transvalues in the memory context aggstate->aggcontext,
 *	  which is also used for the hashtable structures in AGG_HASHED mode.
 *	  The node's regular econtext (aggstate->csstate.cstate.cs_ExprContext)
 *	  is used to run finalize functions and compute the output tuple;
 *	  this context can be reset once per output tuple.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeAgg.c,v 1.94 2002/11/11 03:02:18 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeGroup.h"
#include "executor/nodeHash.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"
#include "utils/datum.h"


/*
 * AggStatePerAggData - per-aggregate working state for the Agg scan
 */
typedef struct AggStatePerAggData
{
	/*
	 * These values are set up during ExecInitAgg() and do not change
	 * thereafter:
	 */

	/* Link to Aggref node this working state is for */
	Aggref	   *aggref;

	/* Oids of transfer functions */
	Oid			transfn_oid;
	Oid			finalfn_oid;	/* may be InvalidOid */

	/*
	 * fmgr lookup data for transfer functions --- only valid when
	 * corresponding oid is not InvalidOid.  Note in particular that
	 * fn_strict flags are kept here.
	 */
	FmgrInfo	transfn;
	FmgrInfo	finalfn;

	/*
	 * Type of input data and Oid of sort operator to use for it; only
	 * set/used when aggregate has DISTINCT flag.  (These are not used
	 * directly by nodeAgg, but must be passed to the Tuplesort object.)
	 */
	Oid			inputType;
	Oid			sortOperator;

	/*
	 * fmgr lookup data for input type's equality operator --- only
	 * set/used when aggregate has DISTINCT flag.
	 */
	FmgrInfo	equalfn;

	/*
	 * initial value from pg_aggregate entry
	 */
	Datum		initValue;
	bool		initValueIsNull;

	/*
	 * We need the len and byval info for the agg's input, result, and
	 * transition data types in order to know how to copy/delete values.
	 */
	int16		inputtypeLen,
				resulttypeLen,
				transtypeLen;
	bool		inputtypeByVal,
				resulttypeByVal,
				transtypeByVal;

	/*
	 * These values are working state that is initialized at the start of
	 * an input tuple group and updated for each input tuple.
	 *
	 * For a simple (non DISTINCT) aggregate, we just feed the input values
	 * straight to the transition function.  If it's DISTINCT, we pass the
	 * input values into a Tuplesort object; then at completion of the
	 * input tuple group, we scan the sorted values, eliminate duplicates,
	 * and run the transition function on the rest.
	 */

	Tuplesortstate *sortstate;	/* sort object, if a DISTINCT agg */
} AggStatePerAggData;

/*
 * AggStatePerGroupData - per-aggregate-per-group working state
 *
 * These values are working state that is initialized at the start of
 * an input tuple group and updated for each input tuple.
 *
 * In AGG_PLAIN and AGG_SORTED modes, we have a single array of these
 * structs (pointed to by aggstate->pergroup); we re-use the array for
 * each input group, if it's AGG_SORTED mode.  In AGG_HASHED mode, the
 * hash table contains an array of these structs for each tuple group.
 *
 * Logically, the sortstate field belongs in this struct, but we do not
 * keep it here for space reasons: we don't support DISTINCT aggregates
 * in AGG_HASHED mode, so there's no reason to use up a pointer field
 * in every entry of the hashtable.
 */
typedef struct AggStatePerGroupData
{
	Datum		transValue;		/* current transition value */
	bool		transValueIsNull;

	bool		noTransValue;	/* true if transValue not set yet */

	/*
	 * Note: noTransValue initially has the same value as
	 * transValueIsNull, and if true both are cleared to false at the same
	 * time.  They are not the same though: if transfn later returns a
	 * NULL, we want to keep that NULL and not auto-replace it with a
	 * later input value. Only the first non-NULL input will be
	 * auto-substituted.
	 */
} AggStatePerGroupData;

/*
 * To implement hashed aggregation, we need a hashtable that stores a
 * representative tuple and an array of AggStatePerGroup structs for each
 * distinct set of GROUP BY column values.  We compute the hash key from
 * the GROUP BY columns.
 */
typedef struct AggHashEntryData
{
	AggHashEntry	next;		/* next entry in same hash bucket */
	uint32		hashkey;		/* exact hash key of this entry */
	HeapTuple	firstTuple;		/* copy of first tuple in this group */
	/* per-aggregate transition status array - must be last! */
	AggStatePerGroupData pergroup[1];	/* VARIABLE LENGTH ARRAY */
} AggHashEntryData;				/* VARIABLE LENGTH STRUCT */

typedef struct AggHashTableData
{
	int			nbuckets;		/* number of buckets in hash table */
	AggHashEntry buckets[1];	/* VARIABLE LENGTH ARRAY */
} AggHashTableData;				/* VARIABLE LENGTH STRUCT */


static void initialize_aggregates(AggState *aggstate,
								  AggStatePerAgg peragg,
								  AggStatePerGroup pergroup);
static void advance_transition_function(AggState *aggstate,
										AggStatePerAgg peraggstate,
										AggStatePerGroup pergroupstate,
										Datum newVal, bool isNull);
static void advance_aggregates(AggState *aggstate, AggStatePerGroup pergroup);
static void process_sorted_aggregate(AggState *aggstate,
									 AggStatePerAgg peraggstate,
									 AggStatePerGroup pergroupstate);
static void finalize_aggregate(AggState *aggstate,
							   AggStatePerAgg peraggstate,
							   AggStatePerGroup pergroupstate,
							   Datum *resultVal, bool *resultIsNull);
static void build_hash_table(Agg *node);
static AggHashEntry lookup_hash_entry(Agg *node, TupleTableSlot *slot);
static TupleTableSlot *agg_retrieve_direct(Agg *node);
static void agg_fill_hash_table(Agg *node);
static TupleTableSlot *agg_retrieve_hash_table(Agg *node);
static Datum GetAggInitVal(Datum textInitVal, Oid transtype);


/*
 * Initialize all aggregates for a new group of input values.
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
static void
initialize_aggregates(AggState *aggstate,
					  AggStatePerAgg peragg,
					  AggStatePerGroup pergroup)
{
	int			aggno;

	for (aggno = 0; aggno < aggstate->numaggs; aggno++)
	{
		AggStatePerAgg peraggstate = &peragg[aggno];
		AggStatePerGroup pergroupstate = &pergroup[aggno];
		Aggref	   *aggref = peraggstate->aggref;

		/*
		 * Start a fresh sort operation for each DISTINCT aggregate.
		 */
		if (aggref->aggdistinct)
		{
			/*
			 * In case of rescan, maybe there could be an uncompleted sort
			 * operation?  Clean it up if so.
			 */
			if (peraggstate->sortstate)
				tuplesort_end(peraggstate->sortstate);

			peraggstate->sortstate =
				tuplesort_begin_datum(peraggstate->inputType,
									  peraggstate->sortOperator,
									  false);
		}

		/*
		 * (Re)set transValue to the initial value.
		 *
		 * Note that when the initial value is pass-by-ref, we must copy it
		 * (into the aggcontext) since we will pfree the transValue later.
		 */
		if (peraggstate->initValueIsNull)
			pergroupstate->transValue = peraggstate->initValue;
		else
		{
			MemoryContext oldContext;

			oldContext = MemoryContextSwitchTo(aggstate->aggcontext);
			pergroupstate->transValue = datumCopy(peraggstate->initValue,
												  peraggstate->transtypeByVal,
												  peraggstate->transtypeLen);
			MemoryContextSwitchTo(oldContext);
		}
		pergroupstate->transValueIsNull = peraggstate->initValueIsNull;

		/*
		 * If the initial value for the transition state doesn't exist in the
		 * pg_aggregate table then we will let the first non-NULL value
		 * returned from the outer procNode become the initial value. (This is
		 * useful for aggregates like max() and min().)  The noTransValue flag
		 * signals that we still need to do this.
		 */
		pergroupstate->noTransValue = peraggstate->initValueIsNull;
	}
}

/*
 * Given a new input value, advance the transition function of an aggregate.
 *
 * It doesn't matter which memory context this is called in.
 */
static void
advance_transition_function(AggState *aggstate,
							AggStatePerAgg peraggstate,
							AggStatePerGroup pergroupstate,
							Datum newVal, bool isNull)
{
	FunctionCallInfoData fcinfo;
	MemoryContext oldContext;

	if (peraggstate->transfn.fn_strict)
	{
		/*
		 * For a strict transfn, nothing happens at a NULL input
		 * tuple; we just keep the prior transValue.
		 */
		if (isNull)
			return;
		if (pergroupstate->noTransValue)
		{
			/*
			 * transValue has not been initialized. This is the first
			 * non-NULL input value. We use it as the initial value for
			 * transValue. (We already checked that the agg's input type
			 * is binary-compatible with its transtype, so straight copy
			 * here is OK.)
			 *
			 * We must copy the datum into aggcontext if it is pass-by-ref.
			 * We do not need to pfree the old transValue, since it's NULL.
			 */
			oldContext = MemoryContextSwitchTo(aggstate->aggcontext);
			pergroupstate->transValue = datumCopy(newVal,
												  peraggstate->transtypeByVal,
												  peraggstate->transtypeLen);
			pergroupstate->transValueIsNull = false;
			pergroupstate->noTransValue = false;
			MemoryContextSwitchTo(oldContext);
			return;
		}
		if (pergroupstate->transValueIsNull)
		{
			/*
			 * Don't call a strict function with NULL inputs.  Note it is
			 * possible to get here despite the above tests, if the
			 * transfn is strict *and* returned a NULL on a prior cycle.
			 * If that happens we will propagate the NULL all the way to
			 * the end.
			 */
			return;
		}
	}

	/* We run the transition functions in per-input-tuple memory context */
	oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);

	/*
	 * OK to call the transition function
	 *
	 * This is heavily-used code, so manually zero just the necessary fields
	 * instead of using MemSet().  Compare FunctionCall2().
	 */

	/* MemSet(&fcinfo, 0, sizeof(fcinfo)); */
	fcinfo.context = NULL;
	fcinfo.resultinfo = NULL;
	fcinfo.isnull = false;

	fcinfo.flinfo = &peraggstate->transfn;
	fcinfo.nargs = 2;
	fcinfo.arg[0] = pergroupstate->transValue;
	fcinfo.argnull[0] = pergroupstate->transValueIsNull;
	fcinfo.arg[1] = newVal;
	fcinfo.argnull[1] = isNull;

	newVal = FunctionCallInvoke(&fcinfo);

	/*
	 * If pass-by-ref datatype, must copy the new value into aggcontext and
	 * pfree the prior transValue.  But if transfn returned a pointer to its
	 * first input, we don't need to do anything.
	 */
	if (!peraggstate->transtypeByVal &&
		DatumGetPointer(newVal) != DatumGetPointer(pergroupstate->transValue))
	{
		if (!fcinfo.isnull)
		{
			MemoryContextSwitchTo(aggstate->aggcontext);
			newVal = datumCopy(newVal,
							   peraggstate->transtypeByVal,
							   peraggstate->transtypeLen);
		}
		if (!pergroupstate->transValueIsNull)
			pfree(DatumGetPointer(pergroupstate->transValue));
	}

	pergroupstate->transValue = newVal;
	pergroupstate->transValueIsNull = fcinfo.isnull;

	MemoryContextSwitchTo(oldContext);
}

/*
 * Advance all the aggregates for one input tuple.  The input tuple
 * has been stored in tmpcontext->ecxt_scantuple, so that it is accessible
 * to ExecEvalExpr.  pergroup is the array of per-group structs to use
 * (this might be in a hashtable entry).
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
static void
advance_aggregates(AggState *aggstate, AggStatePerGroup pergroup)
{
	ExprContext *econtext = aggstate->tmpcontext;
	int			aggno;

	for (aggno = 0; aggno < aggstate->numaggs; aggno++)
	{
		AggStatePerAgg peraggstate = &aggstate->peragg[aggno];
		AggStatePerGroup pergroupstate = &pergroup[aggno];
		Aggref	   *aggref = peraggstate->aggref;
		Datum		newVal;
		bool		isNull;

		newVal = ExecEvalExprSwitchContext(aggref->target, econtext,
										   &isNull, NULL);

		if (aggref->aggdistinct)
		{
			/* in DISTINCT mode, we may ignore nulls */
			if (isNull)
				continue;
			tuplesort_putdatum(peraggstate->sortstate, newVal, isNull);
		}
		else
		{
			advance_transition_function(aggstate, peraggstate, pergroupstate,
										newVal, isNull);
		}
	}
}

/*
 * Run the transition function for a DISTINCT aggregate.  This is called
 * after we have completed entering all the input values into the sort
 * object.	We complete the sort, read out the values in sorted order,
 * and run the transition function on each non-duplicate value.
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
static void
process_sorted_aggregate(AggState *aggstate,
						 AggStatePerAgg peraggstate,
						 AggStatePerGroup pergroupstate)
{
	Datum		oldVal = (Datum) 0;
	bool		haveOldVal = false;
	MemoryContext workcontext = aggstate->tmpcontext->ecxt_per_tuple_memory;
	MemoryContext oldContext;
	Datum		newVal;
	bool		isNull;

	tuplesort_performsort(peraggstate->sortstate);

	/*
	 * Note: if input type is pass-by-ref, the datums returned by the sort
	 * are freshly palloc'd in the per-query context, so we must be
	 * careful to pfree them when they are no longer needed.
	 */

	while (tuplesort_getdatum(peraggstate->sortstate, true,
							  &newVal, &isNull))
	{
		/*
		 * DISTINCT always suppresses nulls, per SQL spec, regardless of
		 * the transition function's strictness.
		 */
		if (isNull)
			continue;

		/*
		 * Clear and select the working context for evaluation of
		 * the equality function and transition function.
		 */
		MemoryContextReset(workcontext);
		oldContext = MemoryContextSwitchTo(workcontext);

		if (haveOldVal &&
			DatumGetBool(FunctionCall2(&peraggstate->equalfn,
									   oldVal, newVal)))
		{
			/* equal to prior, so forget this one */
			if (!peraggstate->inputtypeByVal)
				pfree(DatumGetPointer(newVal));
		}
		else
		{
			advance_transition_function(aggstate, peraggstate, pergroupstate,
										newVal, false);
			/* forget the old value, if any */
			if (haveOldVal && !peraggstate->inputtypeByVal)
				pfree(DatumGetPointer(oldVal));
			/* and remember the new one for subsequent equality checks */
			oldVal = newVal;
			haveOldVal = true;
		}

		MemoryContextSwitchTo(oldContext);
	}

	if (haveOldVal && !peraggstate->inputtypeByVal)
		pfree(DatumGetPointer(oldVal));

	tuplesort_end(peraggstate->sortstate);
	peraggstate->sortstate = NULL;
}

/*
 * Compute the final value of one aggregate.
 *
 * The finalfunction will be run, and the result delivered, in the
 * output-tuple context; caller's CurrentMemoryContext does not matter.
 */
static void
finalize_aggregate(AggState *aggstate,
				   AggStatePerAgg peraggstate,
				   AggStatePerGroup pergroupstate,
				   Datum *resultVal, bool *resultIsNull)
{
	MemoryContext oldContext;

	oldContext = MemoryContextSwitchTo(aggstate->csstate.cstate.cs_ExprContext->ecxt_per_tuple_memory);

	/*
	 * Apply the agg's finalfn if one is provided, else return transValue.
	 */
	if (OidIsValid(peraggstate->finalfn_oid))
	{
		FunctionCallInfoData fcinfo;

		MemSet(&fcinfo, 0, sizeof(fcinfo));
		fcinfo.flinfo = &peraggstate->finalfn;
		fcinfo.nargs = 1;
		fcinfo.arg[0] = pergroupstate->transValue;
		fcinfo.argnull[0] = pergroupstate->transValueIsNull;
		if (fcinfo.flinfo->fn_strict && pergroupstate->transValueIsNull)
		{
			/* don't call a strict function with NULL inputs */
			*resultVal = (Datum) 0;
			*resultIsNull = true;
		}
		else
		{
			*resultVal = FunctionCallInvoke(&fcinfo);
			*resultIsNull = fcinfo.isnull;
		}
	}
	else
	{
		*resultVal = pergroupstate->transValue;
		*resultIsNull = pergroupstate->transValueIsNull;
	}

	/*
	 * If result is pass-by-ref, make sure it is in the right context.
	 */
	if (!peraggstate->resulttypeByVal && !*resultIsNull &&
		!MemoryContextContains(CurrentMemoryContext,
							   DatumGetPointer(*resultVal)))
		*resultVal = datumCopy(*resultVal,
							   peraggstate->resulttypeByVal,
							   peraggstate->resulttypeLen);

	MemoryContextSwitchTo(oldContext);
}

/*
 * Initialize the hash table to empty.
 *
 * The hash table always lives in the aggcontext memory context.
 */
static void
build_hash_table(Agg *node)
{
	AggState   *aggstate = node->aggstate;
	AggHashTable	hashtable;
	Size			tabsize;

	Assert(node->aggstrategy == AGG_HASHED);
	Assert(node->numGroups > 0);
	tabsize = sizeof(AggHashTableData) +
		(node->numGroups - 1) * sizeof(AggHashEntry);
	hashtable = (AggHashTable) MemoryContextAlloc(aggstate->aggcontext,
												  tabsize);
	MemSet(hashtable, 0, tabsize);
	hashtable->nbuckets = node->numGroups;
	aggstate->hashtable = hashtable;
}

/*
 * Find or create a hashtable entry for the tuple group containing the
 * given tuple.
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
static AggHashEntry
lookup_hash_entry(Agg *node, TupleTableSlot *slot)
{
	AggState   *aggstate = node->aggstate;
	AggHashTable hashtable = aggstate->hashtable;
	MemoryContext	tmpmem = aggstate->tmpcontext->ecxt_per_tuple_memory;
	HeapTuple	tuple = slot->val;
	TupleDesc	tupdesc = slot->ttc_tupleDescriptor;
	uint32		hashkey = 0;
	int			i;
	int			bucketno;
	AggHashEntry	entry;
	MemoryContext oldContext;
	Size		entrysize;

	/* Need to run the hash function in short-lived context */
	oldContext = MemoryContextSwitchTo(tmpmem);

	for (i = 0; i < node->numCols; i++)
	{
		AttrNumber	att = node->grpColIdx[i];
		Datum		attr;
		bool		isNull;

		attr = heap_getattr(tuple, att, tupdesc, &isNull);
		if (isNull)
			continue;			/* treat nulls as having hash key 0 */
		hashkey ^= ComputeHashFunc(attr,
								   (int) tupdesc->attrs[att - 1]->attlen,
								   tupdesc->attrs[att - 1]->attbyval);
	}
	bucketno = hashkey % (uint32) hashtable->nbuckets;

	for (entry = hashtable->buckets[bucketno];
		 entry != NULL;
		 entry = entry->next)
	{
		/* Quick check using hashkey */
		if (entry->hashkey != hashkey)
			continue;
		if (execTuplesMatch(entry->firstTuple,
							tuple,
							tupdesc,
							node->numCols, node->grpColIdx,
							aggstate->eqfunctions,
							tmpmem))
		{
			MemoryContextSwitchTo(oldContext);
			return entry;
		}
	}

	/* Not there, so build a new one */
	MemoryContextSwitchTo(aggstate->aggcontext);
	entrysize = sizeof(AggHashEntryData) +
		(aggstate->numaggs - 1) * sizeof(AggStatePerGroupData);
	entry = (AggHashEntry) palloc(entrysize);
	MemSet(entry, 0, entrysize);

	entry->hashkey = hashkey;
	entry->firstTuple = heap_copytuple(tuple);

	entry->next = hashtable->buckets[bucketno];
	hashtable->buckets[bucketno] = entry;

	MemoryContextSwitchTo(oldContext);

	/* initialize aggregates for new tuple group */
	initialize_aggregates(aggstate, aggstate->peragg, entry->pergroup);

	return entry;
}

/*
 * ExecAgg -
 *
 *	  ExecAgg receives tuples from its outer subplan and aggregates over
 *	  the appropriate attribute for each aggregate function use (Aggref
 *	  node) appearing in the targetlist or qual of the node.  The number
 *	  of tuples to aggregate over depends on whether grouped or plain
 *	  aggregation is selected.  In grouped aggregation, we produce a result
 *	  row for each group; in plain aggregation there's a single result row
 *	  for the whole query.  In either case, the value of each aggregate is
 *	  stored in the expression context to be used when ExecProject evaluates
 *	  the result tuple.
 */
TupleTableSlot *
ExecAgg(Agg *node)
{
	AggState   *aggstate = node->aggstate;

	if (aggstate->agg_done)
		return NULL;

	if (node->aggstrategy == AGG_HASHED)
	{
		if (!aggstate->table_filled)
			agg_fill_hash_table(node);
		return agg_retrieve_hash_table(node);
	}
	else
	{
		return agg_retrieve_direct(node);
	}
}

/*
 * ExecAgg for non-hashed case
 */
static TupleTableSlot *
agg_retrieve_direct(Agg *node)
{
	AggState   *aggstate;
	Plan	   *outerPlan;
	ExprContext *econtext;
	ExprContext *tmpcontext;
	ProjectionInfo *projInfo;
	Datum	   *aggvalues;
	bool	   *aggnulls;
	AggStatePerAgg peragg;
	AggStatePerGroup pergroup;
	TupleTableSlot *outerslot;
	TupleTableSlot *firstSlot;
	TupleTableSlot *resultSlot;
	int			aggno;

	/*
	 * get state info from node
	 */
	aggstate = node->aggstate;
	outerPlan = outerPlan(node);
	/* econtext is the per-output-tuple expression context */
	econtext = aggstate->csstate.cstate.cs_ExprContext;
	aggvalues = econtext->ecxt_aggvalues;
	aggnulls = econtext->ecxt_aggnulls;
	/* tmpcontext is the per-input-tuple expression context */
	tmpcontext = aggstate->tmpcontext;
	projInfo = aggstate->csstate.cstate.cs_ProjInfo;
	peragg = aggstate->peragg;
	pergroup = aggstate->pergroup;
	firstSlot = aggstate->csstate.css_ScanTupleSlot;

	/*
	 * We loop retrieving groups until we find one matching
	 * node->plan.qual
	 */
	do
	{
		if (aggstate->agg_done)
			return NULL;

		/*
		 * If we don't already have the first tuple of the new group,
		 * fetch it from the outer plan.
		 */
		if (aggstate->grp_firstTuple == NULL)
		{
			outerslot = ExecProcNode(outerPlan, (Plan *) node);
			if (!TupIsNull(outerslot))
			{
				/*
				 * Make a copy of the first input tuple; we will use this
				 * for comparisons (in group mode) and for projection.
				 */
				aggstate->grp_firstTuple = heap_copytuple(outerslot->val);
			}
			else
			{
				/* outer plan produced no tuples at all */
				aggstate->agg_done = true;
				/* If we are grouping, we should produce no tuples too */
				if (node->aggstrategy != AGG_PLAIN)
					return NULL;
			}
		}

		/*
		 * Clear the per-output-tuple context for each group
		 */
		ResetExprContext(econtext);

		/*
		 * Initialize working state for a new input tuple group
		 */
		initialize_aggregates(aggstate, peragg, pergroup);

		if (aggstate->grp_firstTuple != NULL)
		{
			/*
			 * Store the copied first input tuple in the tuple table slot
			 * reserved for it.  The tuple will be deleted when it is
			 * cleared from the slot.
			 */
			ExecStoreTuple(aggstate->grp_firstTuple,
						   firstSlot,
						   InvalidBuffer,
						   true);
			aggstate->grp_firstTuple = NULL; /* don't keep two pointers */

			/* set up for first advance_aggregates call */
			tmpcontext->ecxt_scantuple = firstSlot;

			/*
			 * Process each outer-plan tuple, and then fetch the next one,
			 * until we exhaust the outer plan or cross a group boundary.
			 */
			for (;;)
			{
				advance_aggregates(aggstate, pergroup);

				/* Reset per-input-tuple context after each tuple */
				ResetExprContext(tmpcontext);

				outerslot = ExecProcNode(outerPlan, (Plan *) node);
				if (TupIsNull(outerslot))
				{
					/* no more outer-plan tuples available */
					aggstate->agg_done = true;
					break;
				}
				/* set up for next advance_aggregates call */
				tmpcontext->ecxt_scantuple = outerslot;

				/*
				 * If we are grouping, check whether we've crossed a group
				 * boundary.
				 */
				if (node->aggstrategy == AGG_SORTED)
				{
					if (!execTuplesMatch(firstSlot->val,
										 outerslot->val,
										 firstSlot->ttc_tupleDescriptor,
										 node->numCols, node->grpColIdx,
										 aggstate->eqfunctions,
										 tmpcontext->ecxt_per_tuple_memory))
					{
						/*
						 * Save the first input tuple of the next group.
						 */
						aggstate->grp_firstTuple = heap_copytuple(outerslot->val);
						break;
					}
				}
			}
		}

		/*
		 * Done scanning input tuple group. Finalize each aggregate
		 * calculation, and stash results in the per-output-tuple context.
		 */
		for (aggno = 0; aggno < aggstate->numaggs; aggno++)
		{
			AggStatePerAgg peraggstate = &peragg[aggno];
			AggStatePerGroup pergroupstate = &pergroup[aggno];

			if (peraggstate->aggref->aggdistinct)
				process_sorted_aggregate(aggstate, peraggstate, pergroupstate);

			finalize_aggregate(aggstate, peraggstate, pergroupstate,
							   &aggvalues[aggno], &aggnulls[aggno]);
		}

		/*
		 * If we have no first tuple (ie, the outerPlan didn't return
		 * anything), create a dummy all-nulls input tuple for use by
		 * ExecProject. 99.44% of the time this is a waste of cycles,
		 * because ordinarily the projected output tuple's targetlist
		 * cannot contain any direct (non-aggregated) references to
		 * input columns, so the dummy tuple will not be referenced.
		 * However there are special cases where this isn't so --- in
		 * particular an UPDATE involving an aggregate will have a
		 * targetlist reference to ctid.  We need to return a null for
		 * ctid in that situation, not coredump.
		 *
		 * The values returned for the aggregates will be the initial
		 * values of the transition functions.
		 */
		if (TupIsNull(firstSlot))
		{
			TupleDesc	tupType;

			/* Should only happen in non-grouped mode */
			Assert(node->aggstrategy == AGG_PLAIN);
			Assert(aggstate->agg_done);

			tupType = firstSlot->ttc_tupleDescriptor;
			/* watch out for zero-column input tuples, though... */
			if (tupType && tupType->natts > 0)
			{
				HeapTuple	nullsTuple;
				Datum	   *dvalues;
				char	   *dnulls;

				dvalues = (Datum *) palloc(sizeof(Datum) * tupType->natts);
				dnulls = (char *) palloc(sizeof(char) * tupType->natts);
				MemSet(dvalues, 0, sizeof(Datum) * tupType->natts);
				MemSet(dnulls, 'n', sizeof(char) * tupType->natts);
				nullsTuple = heap_formtuple(tupType, dvalues, dnulls);
				ExecStoreTuple(nullsTuple,
							   firstSlot,
							   InvalidBuffer,
							   true);
				pfree(dvalues);
				pfree(dnulls);
			}
		}

		/*
		 * Form a projection tuple using the aggregate results and the
		 * representative input tuple.	Store it in the result tuple slot.
		 * Note we do not support aggregates returning sets ...
		 */
		econtext->ecxt_scantuple = firstSlot;
		resultSlot = ExecProject(projInfo, NULL);

		/*
		 * If the completed tuple does not match the qualifications, it is
		 * ignored and we loop back to try to process another group.
		 * Otherwise, return the tuple.
		 */
	}
	while (!ExecQual(node->plan.qual, econtext, false));

	return resultSlot;
}

/*
 * ExecAgg for hashed case: phase 1, read input and build hash table
 */
static void
agg_fill_hash_table(Agg *node)
{
	AggState   *aggstate;
	Plan	   *outerPlan;
	ExprContext *tmpcontext;
	AggHashEntry	entry;
	TupleTableSlot *outerslot;

	/*
	 * get state info from node
	 */
	aggstate = node->aggstate;
	outerPlan = outerPlan(node);
	/* tmpcontext is the per-input-tuple expression context */
	tmpcontext = aggstate->tmpcontext;

	/*
	 * Process each outer-plan tuple, and then fetch the next one,
	 * until we exhaust the outer plan.
	 */
	for (;;)
	{
		outerslot = ExecProcNode(outerPlan, (Plan *) node);
		if (TupIsNull(outerslot))
			break;
		/* set up for advance_aggregates call */
		tmpcontext->ecxt_scantuple = outerslot;

		/* Find or build hashtable entry for this tuple's group */
		entry = lookup_hash_entry(node, outerslot);

		/* Advance the aggregates */
		advance_aggregates(aggstate, entry->pergroup);

		/* Reset per-input-tuple context after each tuple */
		ResetExprContext(tmpcontext);
	}

	aggstate->table_filled = true;
	/* Initialize to walk the hash table */
	aggstate->next_hash_entry = NULL;
	aggstate->next_hash_bucket = 0;
}

/*
 * ExecAgg for hashed case: phase 2, retrieving groups from hash table
 */
static TupleTableSlot *
agg_retrieve_hash_table(Agg *node)
{
	AggState   *aggstate;
	ExprContext *econtext;
	ProjectionInfo *projInfo;
	Datum	   *aggvalues;
	bool	   *aggnulls;
	AggStatePerAgg peragg;
	AggStatePerGroup pergroup;
	AggHashTable	hashtable;
	AggHashEntry	entry;
	TupleTableSlot *firstSlot;
	TupleTableSlot *resultSlot;
	int			aggno;

	/*
	 * get state info from node
	 */
	aggstate = node->aggstate;
	/* econtext is the per-output-tuple expression context */
	econtext = aggstate->csstate.cstate.cs_ExprContext;
	aggvalues = econtext->ecxt_aggvalues;
	aggnulls = econtext->ecxt_aggnulls;
	projInfo = aggstate->csstate.cstate.cs_ProjInfo;
	peragg = aggstate->peragg;
	hashtable = aggstate->hashtable;
	firstSlot = aggstate->csstate.css_ScanTupleSlot;

	/*
	 * We loop retrieving groups until we find one matching
	 * node->plan.qual
	 */
	do
	{
		if (aggstate->agg_done)
			return NULL;

		/*
		 * Find the next entry in the hash table
		 */
		entry = aggstate->next_hash_entry;
		while (entry == NULL)
		{
			if (aggstate->next_hash_bucket >= hashtable->nbuckets)
			{
				/* No more entries in hashtable, so done */
				aggstate->agg_done = TRUE;
				return NULL;
			}
			entry = hashtable->buckets[aggstate->next_hash_bucket++];
		}
		aggstate->next_hash_entry = entry->next;

		/*
		 * Clear the per-output-tuple context for each group
		 */
		ResetExprContext(econtext);

		/*
		 * Store the copied first input tuple in the tuple table slot
		 * reserved for it, so that it can be used in ExecProject.
		 */
		ExecStoreTuple(entry->firstTuple,
					   firstSlot,
					   InvalidBuffer,
					   false);

		pergroup = entry->pergroup;

		/*
		 * Finalize each aggregate calculation, and stash results in the
		 * per-output-tuple context.
		 */
		for (aggno = 0; aggno < aggstate->numaggs; aggno++)
		{
			AggStatePerAgg peraggstate = &peragg[aggno];
			AggStatePerGroup pergroupstate = &pergroup[aggno];

			Assert(!peraggstate->aggref->aggdistinct);
			finalize_aggregate(aggstate, peraggstate, pergroupstate,
							   &aggvalues[aggno], &aggnulls[aggno]);
		}

		/*
		 * Form a projection tuple using the aggregate results and the
		 * representative input tuple.	Store it in the result tuple slot.
		 * Note we do not support aggregates returning sets ...
		 */
		econtext->ecxt_scantuple = firstSlot;
		resultSlot = ExecProject(projInfo, NULL);

		/*
		 * If the completed tuple does not match the qualifications, it is
		 * ignored and we loop back to try to process another group.
		 * Otherwise, return the tuple.
		 */
	}
	while (!ExecQual(node->plan.qual, econtext, false));

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
	AggState   *aggstate;
	AggStatePerAgg peragg;
	Plan	   *outerPlan;
	ExprContext *econtext;
	int			numaggs,
				aggno;
	List	   *alist;

	/*
	 * assign the node's execution state
	 */
	node->plan.state = estate;

	/*
	 * create state structure
	 */
	aggstate = makeNode(AggState);
	node->aggstate = aggstate;
	aggstate->eqfunctions = NULL;
	aggstate->peragg = NULL;
	aggstate->agg_done = false;
	aggstate->pergroup = NULL;
	aggstate->grp_firstTuple = NULL;
	aggstate->hashtable = NULL;

	/*
	 * find aggregates in targetlist and quals
	 *
	 * Note: pull_agg_clauses also checks that no aggs contain other agg
	 * calls in their arguments.  This would make no sense under SQL
	 * semantics anyway (and it's forbidden by the spec).  Because that is
	 * true, we don't need to worry about evaluating the aggs in any
	 * particular order.
	 */
	aggstate->aggs = nconc(pull_agg_clause((Node *) node->plan.targetlist),
						   pull_agg_clause((Node *) node->plan.qual));
	aggstate->numaggs = numaggs = length(aggstate->aggs);
	if (numaggs <= 0)
	{
		/*
		 * This is not an error condition: we might be using the Agg node just
		 * to do hash-based grouping.  Even in the regular case,
		 * constant-expression simplification could optimize away all of the
		 * Aggrefs in the targetlist and qual.  So keep going, but force local
		 * copy of numaggs positive so that palloc()s below don't choke.
		 */
		numaggs = 1;
	}

	/*
	 * Create expression contexts.  We need two, one for per-input-tuple
	 * processing and one for per-output-tuple processing.  We cheat a little
	 * by using ExecAssignExprContext() to build both.
	 */
	ExecAssignExprContext(estate, &aggstate->csstate.cstate);
	aggstate->tmpcontext = aggstate->csstate.cstate.cs_ExprContext;
	ExecAssignExprContext(estate, &aggstate->csstate.cstate);

	/*
	 * We also need a long-lived memory context for holding hashtable
	 * data structures and transition values.  NOTE: the details of what
	 * is stored in aggcontext and what is stored in the regular per-query
	 * memory context are driven by a simple decision: we want to reset the
	 * aggcontext in ExecReScanAgg to recover no-longer-wanted space.
	 */
	aggstate->aggcontext =
		AllocSetContextCreate(CurrentMemoryContext,
							  "AggContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

#define AGG_NSLOTS 2

	/*
	 * tuple table initialization
	 */
	ExecInitScanTupleSlot(estate, &aggstate->csstate);
	ExecInitResultTupleSlot(estate, &aggstate->csstate.cstate);

	/*
	 * Set up aggregate-result storage in the output expr context, and also
	 * allocate my private per-agg working storage
	 */
	econtext = aggstate->csstate.cstate.cs_ExprContext;
	econtext->ecxt_aggvalues = (Datum *) palloc(sizeof(Datum) * numaggs);
	MemSet(econtext->ecxt_aggvalues, 0, sizeof(Datum) * numaggs);
	econtext->ecxt_aggnulls = (bool *) palloc(sizeof(bool) * numaggs);
	MemSet(econtext->ecxt_aggnulls, 0, sizeof(bool) * numaggs);

	peragg = (AggStatePerAgg) palloc(sizeof(AggStatePerAggData) * numaggs);
	MemSet(peragg, 0, sizeof(AggStatePerAggData) * numaggs);
	aggstate->peragg = peragg;

	if (node->aggstrategy == AGG_HASHED)
	{
		build_hash_table(node);
		aggstate->table_filled = false;
	}
	else
	{
		AggStatePerGroup pergroup;

		pergroup = (AggStatePerGroup) palloc(sizeof(AggStatePerGroupData) * numaggs);
		MemSet(pergroup, 0, sizeof(AggStatePerGroupData) * numaggs);
		aggstate->pergroup = pergroup;
	}

	/*
	 * initialize child nodes
	 */
	outerPlan = outerPlan(node);
	ExecInitNode(outerPlan, estate, (Plan *) node);

	/*
	 * initialize source tuple type.
	 */
	ExecAssignScanTypeFromOuterPlan((Plan *) node, &aggstate->csstate);

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL((Plan *) node, &aggstate->csstate.cstate);
	ExecAssignProjectionInfo((Plan *) node, &aggstate->csstate.cstate);

	/*
	 * If we are grouping, precompute fmgr lookup data for inner loop
	 */
	if (node->numCols > 0)
	{
		aggstate->eqfunctions =
			execTuplesMatchPrepare(ExecGetScanType(&aggstate->csstate),
								   node->numCols,
								   node->grpColIdx);
	}

	/*
	 * Perform lookups of aggregate function info, and initialize the
	 * unchanging fields of the per-agg data
	 */
	aggno = -1;
	foreach(alist, aggstate->aggs)
	{
		Aggref	   *aggref = (Aggref *) lfirst(alist);
		AggStatePerAgg peraggstate = &peragg[++aggno];
		HeapTuple	aggTuple;
		Form_pg_aggregate aggform;
		AclResult	aclresult;
		Oid			transfn_oid,
					finalfn_oid;
		Datum		textInitVal;

		/* Mark Aggref node with its associated index in the result array */
		aggref->aggno = aggno;

		/* Fill in the peraggstate data */
		peraggstate->aggref = aggref;

		aggTuple = SearchSysCache(AGGFNOID,
								  ObjectIdGetDatum(aggref->aggfnoid),
								  0, 0, 0);
		if (!HeapTupleIsValid(aggTuple))
			elog(ERROR, "ExecAgg: cache lookup failed for aggregate %u",
				 aggref->aggfnoid);
		aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);

		/* Check permission to call aggregate function */
		aclresult = pg_proc_aclcheck(aggref->aggfnoid, GetUserId(),
									 ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, get_func_name(aggref->aggfnoid));

		get_typlenbyval(aggref->aggtype,
						&peraggstate->resulttypeLen,
						&peraggstate->resulttypeByVal);
		get_typlenbyval(aggform->aggtranstype,
						&peraggstate->transtypeLen,
						&peraggstate->transtypeByVal);

		/*
		 * initval is potentially null, so don't try to access it as a
		 * struct field. Must do it the hard way with SysCacheGetAttr.
		 */
		textInitVal = SysCacheGetAttr(AGGFNOID, aggTuple,
									  Anum_pg_aggregate_agginitval,
									  &peraggstate->initValueIsNull);

		if (peraggstate->initValueIsNull)
			peraggstate->initValue = (Datum) 0;
		else
			peraggstate->initValue = GetAggInitVal(textInitVal,
												   aggform->aggtranstype);

		peraggstate->transfn_oid = transfn_oid = aggform->aggtransfn;
		peraggstate->finalfn_oid = finalfn_oid = aggform->aggfinalfn;

		fmgr_info(transfn_oid, &peraggstate->transfn);
		if (OidIsValid(finalfn_oid))
			fmgr_info(finalfn_oid, &peraggstate->finalfn);

		/*
		 * If the transfn is strict and the initval is NULL, make sure
		 * input type and transtype are the same (or at least binary-
		 * compatible), so that it's OK to use the first input value as
		 * the initial transValue.	This should have been checked at agg
		 * definition time, but just in case...
		 */
		if (peraggstate->transfn.fn_strict && peraggstate->initValueIsNull)
		{
			/*
			 * Note: use the type from the input expression here, not from
			 * pg_proc.proargtypes, because the latter might be 0.
			 * (Consider COUNT(*).)
			 */
			Oid			inputType = exprType(aggref->target);

			if (!IsBinaryCoercible(inputType, aggform->aggtranstype))
				elog(ERROR, "Aggregate %u needs to have compatible input type and transition type",
					 aggref->aggfnoid);
		}

		if (aggref->aggdistinct)
		{
			/*
			 * Note: use the type from the input expression here, not from
			 * pg_proc.proargtypes, because the latter might be a pseudotype.
			 * (Consider COUNT(*).)
			 */
			Oid			inputType = exprType(aggref->target);
			Oid			eq_function;

			/* We don't implement DISTINCT aggs in the HASHED case */
			Assert(node->aggstrategy != AGG_HASHED);

			peraggstate->inputType = inputType;
			get_typlenbyval(inputType,
							&peraggstate->inputtypeLen,
							&peraggstate->inputtypeByVal);

			eq_function = compatible_oper_funcid(makeList1(makeString("=")),
												 inputType, inputType,
												 true);
			if (!OidIsValid(eq_function))
				elog(ERROR, "Unable to identify an equality operator for type %s",
					 format_type_be(inputType));
			fmgr_info(eq_function, &(peraggstate->equalfn));
			peraggstate->sortOperator = any_ordering_op(inputType);
			peraggstate->sortstate = NULL;
		}

		ReleaseSysCache(aggTuple);
	}

	return TRUE;
}

static Datum
GetAggInitVal(Datum textInitVal, Oid transtype)
{
	char	   *strInitVal;
	HeapTuple	tup;
	Oid			typinput,
				typelem;
	Datum		initVal;

	strInitVal = DatumGetCString(DirectFunctionCall1(textout, textInitVal));

	tup = SearchSysCache(TYPEOID,
						 ObjectIdGetDatum(transtype),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "GetAggInitVal: cache lookup failed on aggregate transition function return type %u", transtype);

	typinput = ((Form_pg_type) GETSTRUCT(tup))->typinput;
	typelem = ((Form_pg_type) GETSTRUCT(tup))->typelem;
	ReleaseSysCache(tup);

	initVal = OidFunctionCall3(typinput,
							   CStringGetDatum(strInitVal),
							   ObjectIdGetDatum(typelem),
							   Int32GetDatum(-1));

	pfree(strInitVal);
	return initVal;
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
	int			aggno;

	/* Make sure we have closed any open tuplesorts */
	for (aggno = 0; aggno < aggstate->numaggs; aggno++)
	{
		AggStatePerAgg peraggstate = &aggstate->peragg[aggno];

		if (peraggstate->sortstate)
			tuplesort_end(peraggstate->sortstate);
	}

	ExecFreeProjectionInfo(&aggstate->csstate.cstate);

	/*
	 * Free both the expr contexts.
	 */
	ExecFreeExprContext(&aggstate->csstate.cstate);
	aggstate->csstate.cstate.cs_ExprContext = aggstate->tmpcontext;
	ExecFreeExprContext(&aggstate->csstate.cstate);

	MemoryContextDelete(aggstate->aggcontext);

	outerPlan = outerPlan(node);
	ExecEndNode(outerPlan, (Plan *) node);

	/* clean up tuple table */
	ExecClearTuple(aggstate->csstate.css_ScanTupleSlot);
	if (aggstate->grp_firstTuple != NULL)
	{
		heap_freetuple(aggstate->grp_firstTuple);
		aggstate->grp_firstTuple = NULL;
	}
}

void
ExecReScanAgg(Agg *node, ExprContext *exprCtxt, Plan *parent)
{
	AggState   *aggstate = node->aggstate;
	ExprContext *econtext = aggstate->csstate.cstate.cs_ExprContext;
	int			aggno;

	/* Make sure we have closed any open tuplesorts */
	for (aggno = 0; aggno < aggstate->numaggs; aggno++)
	{
		AggStatePerAgg peraggstate = &aggstate->peragg[aggno];

		if (peraggstate->sortstate)
			tuplesort_end(peraggstate->sortstate);
		peraggstate->sortstate = NULL;
	}

	aggstate->agg_done = false;
	if (aggstate->grp_firstTuple != NULL)
	{
		heap_freetuple(aggstate->grp_firstTuple);
		aggstate->grp_firstTuple = NULL;
	}
	MemSet(econtext->ecxt_aggvalues, 0, sizeof(Datum) * aggstate->numaggs);
	MemSet(econtext->ecxt_aggnulls, 0, sizeof(bool) * aggstate->numaggs);

	MemoryContextReset(aggstate->aggcontext);

	if (node->aggstrategy == AGG_HASHED)
	{
		build_hash_table(node);
		aggstate->table_filled = false;
	}

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((Plan *) node)->lefttree->chgParam == NULL)
		ExecReScan(((Plan *) node)->lefttree, exprCtxt, (Plan *) node);
}

/*
 * aggregate_dummy - dummy execution routine for aggregate functions
 *
 * This function is listed as the implementation (prosrc field) of pg_proc
 * entries for aggregate functions.  Its only purpose is to throw an error
 * if someone mistakenly executes such a function in the normal way.
 *
 * Perhaps someday we could assign real meaning to the prosrc field of
 * an aggregate?
 */
Datum
aggregate_dummy(PG_FUNCTION_ARGS)
{
	elog(ERROR, "Aggregate function %u called as normal function",
		 fcinfo->flinfo->fn_oid);
	return (Datum) 0;			/* keep compiler quiet */
}
