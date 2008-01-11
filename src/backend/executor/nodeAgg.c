/*-------------------------------------------------------------------------
 *
 * nodeAgg.c
 *	  Routines to handle aggregate nodes.
 *
 *	  ExecAgg evaluates each aggregate in the following steps:
 *
 *		 transvalue = initcond
 *		 foreach input_tuple do
 *			transvalue = transfunc(transvalue, input_value(s))
 *		 result = finalfunc(transvalue)
 *
 *	  If a finalfunc is not supplied then the result is just the ending
 *	  value of transvalue.
 *
 *	  If transfunc is marked "strict" in pg_proc and initcond is NULL,
 *	  then the first non-NULL input_value is assigned directly to transvalue,
 *	  and transfunc isn't applied until the second non-NULL input_value.
 *	  The agg's first input type and transtype must be the same in this case!
 *
 *	  If transfunc is marked "strict" then NULL input_values are skipped,
 *	  keeping the previous transvalue.	If transfunc is not strict then it
 *	  is called for every input tuple and must deal with NULL initcond
 *	  or NULL input_values for itself.
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
 *	  Beginning in PostgreSQL 8.1, the executor's AggState node is passed as
 *	  the fmgr "context" value in all transfunc and finalfunc calls.  It is
 *	  not really intended that the transition functions will look into the
 *	  AggState node, but they can use code like
 *			if (fcinfo->context && IsA(fcinfo->context, AggState))
 *	  to verify that they are being called by nodeAgg.c and not as ordinary
 *	  SQL functions.  The main reason a transition function might want to know
 *	  that is that it can avoid palloc'ing a fixed-size pass-by-ref transition
 *	  value on every call: it can instead just scribble on and return its left
 *	  input.  Ordinarily it is completely forbidden for functions to modify
 *	  pass-by-ref inputs, but in the aggregate case we know the left input is
 *	  either the initial transition value or a previous function result, and
 *	  in either case its value need not be preserved.  See int8inc() for an
 *	  example.	Notice that advance_transition_function() is coded to avoid a
 *	  data copy step when the previous transition value pointer is returned.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeAgg.c,v 1.156 2008/01/11 18:39:40 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
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

	/* Links to Aggref expr and state nodes this working state is for */
	AggrefExprState *aggrefstate;
	Aggref	   *aggref;

	/* number of input arguments for aggregate */
	int			numArguments;

	/* Oids of transfer functions */
	Oid			transfn_oid;
	Oid			finalfn_oid;	/* may be InvalidOid */

	/*
	 * fmgr lookup data for transfer functions --- only valid when
	 * corresponding oid is not InvalidOid.  Note in particular that fn_strict
	 * flags are kept here.
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
	 * fmgr lookup data for input type's equality operator --- only set/used
	 * when aggregate has DISTINCT flag.
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
	 * These values are working state that is initialized at the start of an
	 * input tuple group and updated for each input tuple.
	 *
	 * For a simple (non DISTINCT) aggregate, we just feed the input values
	 * straight to the transition function.  If it's DISTINCT, we pass the
	 * input values into a Tuplesort object; then at completion of the input
	 * tuple group, we scan the sorted values, eliminate duplicates, and run
	 * the transition function on the rest.
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
	 * Note: noTransValue initially has the same value as transValueIsNull,
	 * and if true both are cleared to false at the same time.	They are not
	 * the same though: if transfn later returns a NULL, we want to keep that
	 * NULL and not auto-replace it with a later input value. Only the first
	 * non-NULL input will be auto-substituted.
	 */
} AggStatePerGroupData;

/*
 * To implement hashed aggregation, we need a hashtable that stores a
 * representative tuple and an array of AggStatePerGroup structs for each
 * distinct set of GROUP BY column values.	We compute the hash key from
 * the GROUP BY columns.
 */
typedef struct AggHashEntryData *AggHashEntry;

typedef struct AggHashEntryData
{
	TupleHashEntryData shared;	/* common header for hash table entries */
	/* per-aggregate transition status array - must be last! */
	AggStatePerGroupData pergroup[1];	/* VARIABLE LENGTH ARRAY */
} AggHashEntryData;				/* VARIABLE LENGTH STRUCT */


static void initialize_aggregates(AggState *aggstate,
					  AggStatePerAgg peragg,
					  AggStatePerGroup pergroup);
static void advance_transition_function(AggState *aggstate,
							AggStatePerAgg peraggstate,
							AggStatePerGroup pergroupstate,
							FunctionCallInfoData *fcinfo);
static void advance_aggregates(AggState *aggstate, AggStatePerGroup pergroup);
static void process_sorted_aggregate(AggState *aggstate,
						 AggStatePerAgg peraggstate,
						 AggStatePerGroup pergroupstate);
static void finalize_aggregate(AggState *aggstate,
				   AggStatePerAgg peraggstate,
				   AggStatePerGroup pergroupstate,
				   Datum *resultVal, bool *resultIsNull);
static Bitmapset *find_unaggregated_cols(AggState *aggstate);
static bool find_unaggregated_cols_walker(Node *node, Bitmapset **colnos);
static void build_hash_table(AggState *aggstate);
static AggHashEntry lookup_hash_entry(AggState *aggstate,
				  TupleTableSlot *inputslot);
static TupleTableSlot *agg_retrieve_direct(AggState *aggstate);
static void agg_fill_hash_table(AggState *aggstate);
static TupleTableSlot *agg_retrieve_hash_table(AggState *aggstate);
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
									  peraggstate->sortOperator, false,
									  work_mem, false);
		}

		/*
		 * If we are reinitializing after a group boundary, we have to free
		 * any prior transValue to avoid memory leakage.  We must check not
		 * only the isnull flag but whether the pointer is NULL; since
		 * pergroupstate is initialized with palloc0, the initial condition
		 * has isnull = 0 and null pointer.
		 */
		if (!peraggstate->transtypeByVal &&
			!pergroupstate->transValueIsNull &&
			DatumGetPointer(pergroupstate->transValue) != NULL)
			pfree(DatumGetPointer(pergroupstate->transValue));

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
		 * useful for aggregates like max() and min().) The noTransValue flag
		 * signals that we still need to do this.
		 */
		pergroupstate->noTransValue = peraggstate->initValueIsNull;
	}
}

/*
 * Given new input value(s), advance the transition function of an aggregate.
 *
 * The new values (and null flags) have been preloaded into argument positions
 * 1 and up in fcinfo, so that we needn't copy them again to pass to the
 * transition function.  No other fields of fcinfo are assumed valid.
 *
 * It doesn't matter which memory context this is called in.
 */
static void
advance_transition_function(AggState *aggstate,
							AggStatePerAgg peraggstate,
							AggStatePerGroup pergroupstate,
							FunctionCallInfoData *fcinfo)
{
	int			numArguments = peraggstate->numArguments;
	MemoryContext oldContext;
	Datum		newVal;
	int			i;

	if (peraggstate->transfn.fn_strict)
	{
		/*
		 * For a strict transfn, nothing happens when there's a NULL input; we
		 * just keep the prior transValue.
		 */
		for (i = 1; i <= numArguments; i++)
		{
			if (fcinfo->argnull[i])
				return;
		}
		if (pergroupstate->noTransValue)
		{
			/*
			 * transValue has not been initialized. This is the first non-NULL
			 * input value. We use it as the initial value for transValue. (We
			 * already checked that the agg's input type is binary-compatible
			 * with its transtype, so straight copy here is OK.)
			 *
			 * We must copy the datum into aggcontext if it is pass-by-ref. We
			 * do not need to pfree the old transValue, since it's NULL.
			 */
			oldContext = MemoryContextSwitchTo(aggstate->aggcontext);
			pergroupstate->transValue = datumCopy(fcinfo->arg[1],
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
			 * possible to get here despite the above tests, if the transfn is
			 * strict *and* returned a NULL on a prior cycle. If that happens
			 * we will propagate the NULL all the way to the end.
			 */
			return;
		}
	}

	/* We run the transition functions in per-input-tuple memory context */
	oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);

	/*
	 * OK to call the transition function
	 */
	InitFunctionCallInfoData(*fcinfo, &(peraggstate->transfn),
							 numArguments + 1,
							 (void *) aggstate, NULL);
	fcinfo->arg[0] = pergroupstate->transValue;
	fcinfo->argnull[0] = pergroupstate->transValueIsNull;

	newVal = FunctionCallInvoke(fcinfo);

	/*
	 * If pass-by-ref datatype, must copy the new value into aggcontext and
	 * pfree the prior transValue.	But if transfn returned a pointer to its
	 * first input, we don't need to do anything.
	 */
	if (!peraggstate->transtypeByVal &&
		DatumGetPointer(newVal) != DatumGetPointer(pergroupstate->transValue))
	{
		if (!fcinfo->isnull)
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
	pergroupstate->transValueIsNull = fcinfo->isnull;

	MemoryContextSwitchTo(oldContext);
}

/*
 * Advance all the aggregates for one input tuple.	The input tuple
 * has been stored in tmpcontext->ecxt_outertuple, so that it is accessible
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
		AggrefExprState *aggrefstate = peraggstate->aggrefstate;
		Aggref	   *aggref = peraggstate->aggref;
		FunctionCallInfoData fcinfo;
		int			i;
		ListCell   *arg;
		MemoryContext oldContext;

		/* Switch memory context just once for all args */
		oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

		/* Evaluate inputs and save in fcinfo */
		/* We start from 1, since the 0th arg will be the transition value */
		i = 1;
		foreach(arg, aggrefstate->args)
		{
			ExprState  *argstate = (ExprState *) lfirst(arg);

			fcinfo.arg[i] = ExecEvalExpr(argstate, econtext,
										 fcinfo.argnull + i, NULL);
			i++;
		}

		/* Switch back */
		MemoryContextSwitchTo(oldContext);

		if (aggref->aggdistinct)
		{
			/* in DISTINCT mode, we may ignore nulls */
			/* XXX we assume there is only one input column */
			if (fcinfo.argnull[1])
				continue;
			tuplesort_putdatum(peraggstate->sortstate, fcinfo.arg[1],
							   fcinfo.argnull[1]);
		}
		else
		{
			advance_transition_function(aggstate, peraggstate, pergroupstate,
										&fcinfo);
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
	Datum	   *newVal;
	bool	   *isNull;
	FunctionCallInfoData fcinfo;

	tuplesort_performsort(peraggstate->sortstate);

	newVal = fcinfo.arg + 1;
	isNull = fcinfo.argnull + 1;

	/*
	 * Note: if input type is pass-by-ref, the datums returned by the sort are
	 * freshly palloc'd in the per-query context, so we must be careful to
	 * pfree them when they are no longer needed.
	 */

	while (tuplesort_getdatum(peraggstate->sortstate, true,
							  newVal, isNull))
	{
		/*
		 * DISTINCT always suppresses nulls, per SQL spec, regardless of the
		 * transition function's strictness.
		 */
		if (*isNull)
			continue;

		/*
		 * Clear and select the working context for evaluation of the equality
		 * function and transition function.
		 */
		MemoryContextReset(workcontext);
		oldContext = MemoryContextSwitchTo(workcontext);

		if (haveOldVal &&
			DatumGetBool(FunctionCall2(&peraggstate->equalfn,
									   oldVal, *newVal)))
		{
			/* equal to prior, so forget this one */
			if (!peraggstate->inputtypeByVal)
				pfree(DatumGetPointer(*newVal));
		}
		else
		{
			advance_transition_function(aggstate, peraggstate, pergroupstate,
										&fcinfo);
			/* forget the old value, if any */
			if (haveOldVal && !peraggstate->inputtypeByVal)
				pfree(DatumGetPointer(oldVal));
			/* and remember the new one for subsequent equality checks */
			oldVal = *newVal;
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

	oldContext = MemoryContextSwitchTo(aggstate->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

	/*
	 * Apply the agg's finalfn if one is provided, else return transValue.
	 */
	if (OidIsValid(peraggstate->finalfn_oid))
	{
		FunctionCallInfoData fcinfo;

		InitFunctionCallInfoData(fcinfo, &(peraggstate->finalfn), 1,
								 (void *) aggstate, NULL);
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
 * find_unaggregated_cols
 *	  Construct a bitmapset of the column numbers of un-aggregated Vars
 *	  appearing in our targetlist and qual (HAVING clause)
 */
static Bitmapset *
find_unaggregated_cols(AggState *aggstate)
{
	Agg		   *node = (Agg *) aggstate->ss.ps.plan;
	Bitmapset  *colnos;

	colnos = NULL;
	(void) find_unaggregated_cols_walker((Node *) node->plan.targetlist,
										 &colnos);
	(void) find_unaggregated_cols_walker((Node *) node->plan.qual,
										 &colnos);
	return colnos;
}

static bool
find_unaggregated_cols_walker(Node *node, Bitmapset **colnos)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		/* setrefs.c should have set the varno to OUTER */
		Assert(var->varno == OUTER);
		Assert(var->varlevelsup == 0);
		*colnos = bms_add_member(*colnos, var->varattno);
		return false;
	}
	if (IsA(node, Aggref))		/* do not descend into aggregate exprs */
		return false;
	return expression_tree_walker(node, find_unaggregated_cols_walker,
								  (void *) colnos);
}

/*
 * Initialize the hash table to empty.
 *
 * The hash table always lives in the aggcontext memory context.
 */
static void
build_hash_table(AggState *aggstate)
{
	Agg		   *node = (Agg *) aggstate->ss.ps.plan;
	MemoryContext tmpmem = aggstate->tmpcontext->ecxt_per_tuple_memory;
	Size		entrysize;
	Bitmapset  *colnos;
	List	   *collist;
	int			i;

	Assert(node->aggstrategy == AGG_HASHED);
	Assert(node->numGroups > 0);

	entrysize = sizeof(AggHashEntryData) +
		(aggstate->numaggs - 1) *sizeof(AggStatePerGroupData);

	aggstate->hashtable = BuildTupleHashTable(node->numCols,
											  node->grpColIdx,
											  aggstate->eqfunctions,
											  aggstate->hashfunctions,
											  node->numGroups,
											  entrysize,
											  aggstate->aggcontext,
											  tmpmem);

	/*
	 * Create a list of the tuple columns that actually need to be stored in
	 * hashtable entries.  The incoming tuples from the child plan node will
	 * contain grouping columns, other columns referenced in our targetlist
	 * and qual, columns used to compute the aggregate functions, and perhaps
	 * just junk columns we don't use at all.  Only columns of the first two
	 * types need to be stored in the hashtable, and getting rid of the others
	 * can make the table entries significantly smaller.  To avoid messing up
	 * Var numbering, we keep the same tuple descriptor for hashtable entries
	 * as the incoming tuples have, but set unwanted columns to NULL in the
	 * tuples that go into the table.
	 *
	 * To eliminate duplicates, we build a bitmapset of the needed columns,
	 * then convert it to an integer list (cheaper to scan at runtime). The
	 * list is in decreasing order so that the first entry is the largest;
	 * lookup_hash_entry depends on this to use slot_getsomeattrs correctly.
	 *
	 * Note: at present, searching the tlist/qual is not really necessary
	 * since the parser should disallow any unaggregated references to
	 * ungrouped columns.  However, the search will be needed when we add
	 * support for SQL99 semantics that allow use of "functionally dependent"
	 * columns that haven't been explicitly grouped by.
	 */

	/* Find Vars that will be needed in tlist and qual */
	colnos = find_unaggregated_cols(aggstate);
	/* Add in all the grouping columns */
	for (i = 0; i < node->numCols; i++)
		colnos = bms_add_member(colnos, node->grpColIdx[i]);
	/* Convert to list, using lcons so largest element ends up first */
	collist = NIL;
	while ((i = bms_first_member(colnos)) >= 0)
		collist = lcons_int(i, collist);
	aggstate->hash_needed = collist;
}

/*
 * Estimate per-hash-table-entry overhead for the planner.
 *
 * Note that the estimate does not include space for pass-by-reference
 * transition data values, nor for the representative tuple of each group.
 */
Size
hash_agg_entry_size(int numAggs)
{
	Size		entrysize;

	/* This must match build_hash_table */
	entrysize = sizeof(AggHashEntryData) +
		(numAggs - 1) *sizeof(AggStatePerGroupData);
	entrysize = MAXALIGN(entrysize);
	/* Account for hashtable overhead (assuming fill factor = 1) */
	entrysize += 3 * sizeof(void *);
	return entrysize;
}

/*
 * Find or create a hashtable entry for the tuple group containing the
 * given tuple.
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
static AggHashEntry
lookup_hash_entry(AggState *aggstate, TupleTableSlot *inputslot)
{
	TupleTableSlot *hashslot = aggstate->hashslot;
	ListCell   *l;
	AggHashEntry entry;
	bool		isnew;

	/* if first time through, initialize hashslot by cloning input slot */
	if (hashslot->tts_tupleDescriptor == NULL)
	{
		ExecSetSlotDescriptor(hashslot, inputslot->tts_tupleDescriptor);
		/* Make sure all unused columns are NULLs */
		ExecStoreAllNullTuple(hashslot);
	}

	/* transfer just the needed columns into hashslot */
	slot_getsomeattrs(inputslot, linitial_int(aggstate->hash_needed));
	foreach(l, aggstate->hash_needed)
	{
		int			varNumber = lfirst_int(l) - 1;

		hashslot->tts_values[varNumber] = inputslot->tts_values[varNumber];
		hashslot->tts_isnull[varNumber] = inputslot->tts_isnull[varNumber];
	}

	/* find or create the hashtable entry using the filtered tuple */
	entry = (AggHashEntry) LookupTupleHashEntry(aggstate->hashtable,
												hashslot,
												&isnew);

	if (isnew)
	{
		/* initialize aggregates for new tuple group */
		initialize_aggregates(aggstate, aggstate->peragg, entry->pergroup);
	}

	return entry;
}

/*
 * ExecAgg -
 *
 *	  ExecAgg receives tuples from its outer subplan and aggregates over
 *	  the appropriate attribute for each aggregate function use (Aggref
 *	  node) appearing in the targetlist or qual of the node.  The number
 *	  of tuples to aggregate over depends on whether grouped or plain
 *	  aggregation is selected.	In grouped aggregation, we produce a result
 *	  row for each group; in plain aggregation there's a single result row
 *	  for the whole query.	In either case, the value of each aggregate is
 *	  stored in the expression context to be used when ExecProject evaluates
 *	  the result tuple.
 */
TupleTableSlot *
ExecAgg(AggState *node)
{
	if (node->agg_done)
		return NULL;

	if (((Agg *) node->ss.ps.plan)->aggstrategy == AGG_HASHED)
	{
		if (!node->table_filled)
			agg_fill_hash_table(node);
		return agg_retrieve_hash_table(node);
	}
	else
		return agg_retrieve_direct(node);
}

/*
 * ExecAgg for non-hashed case
 */
static TupleTableSlot *
agg_retrieve_direct(AggState *aggstate)
{
	Agg		   *node = (Agg *) aggstate->ss.ps.plan;
	PlanState  *outerPlan;
	ExprContext *econtext;
	ExprContext *tmpcontext;
	ProjectionInfo *projInfo;
	Datum	   *aggvalues;
	bool	   *aggnulls;
	AggStatePerAgg peragg;
	AggStatePerGroup pergroup;
	TupleTableSlot *outerslot;
	TupleTableSlot *firstSlot;
	int			aggno;

	/*
	 * get state info from node
	 */
	outerPlan = outerPlanState(aggstate);
	/* econtext is the per-output-tuple expression context */
	econtext = aggstate->ss.ps.ps_ExprContext;
	aggvalues = econtext->ecxt_aggvalues;
	aggnulls = econtext->ecxt_aggnulls;
	/* tmpcontext is the per-input-tuple expression context */
	tmpcontext = aggstate->tmpcontext;
	projInfo = aggstate->ss.ps.ps_ProjInfo;
	peragg = aggstate->peragg;
	pergroup = aggstate->pergroup;
	firstSlot = aggstate->ss.ss_ScanTupleSlot;

	/*
	 * We loop retrieving groups until we find one matching
	 * aggstate->ss.ps.qual
	 */
	while (!aggstate->agg_done)
	{
		/*
		 * If we don't already have the first tuple of the new group, fetch it
		 * from the outer plan.
		 */
		if (aggstate->grp_firstTuple == NULL)
		{
			outerslot = ExecProcNode(outerPlan);
			if (!TupIsNull(outerslot))
			{
				/*
				 * Make a copy of the first input tuple; we will use this for
				 * comparisons (in group mode) and for projection.
				 */
				aggstate->grp_firstTuple = ExecCopySlotTuple(outerslot);
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
			 * reserved for it.  The tuple will be deleted when it is cleared
			 * from the slot.
			 */
			ExecStoreTuple(aggstate->grp_firstTuple,
						   firstSlot,
						   InvalidBuffer,
						   true);
			aggstate->grp_firstTuple = NULL;	/* don't keep two pointers */

			/* set up for first advance_aggregates call */
			tmpcontext->ecxt_outertuple = firstSlot;

			/*
			 * Process each outer-plan tuple, and then fetch the next one,
			 * until we exhaust the outer plan or cross a group boundary.
			 */
			for (;;)
			{
				advance_aggregates(aggstate, pergroup);

				/* Reset per-input-tuple context after each tuple */
				ResetExprContext(tmpcontext);

				outerslot = ExecProcNode(outerPlan);
				if (TupIsNull(outerslot))
				{
					/* no more outer-plan tuples available */
					aggstate->agg_done = true;
					break;
				}
				/* set up for next advance_aggregates call */
				tmpcontext->ecxt_outertuple = outerslot;

				/*
				 * If we are grouping, check whether we've crossed a group
				 * boundary.
				 */
				if (node->aggstrategy == AGG_SORTED)
				{
					if (!execTuplesMatch(firstSlot,
										 outerslot,
										 node->numCols, node->grpColIdx,
										 aggstate->eqfunctions,
										 tmpcontext->ecxt_per_tuple_memory))
					{
						/*
						 * Save the first input tuple of the next group.
						 */
						aggstate->grp_firstTuple = ExecCopySlotTuple(outerslot);
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
		 * Use the representative input tuple for any references to
		 * non-aggregated input columns in the qual and tlist.	(If we are not
		 * grouping, and there are no input rows at all, we will come here
		 * with an empty firstSlot ... but if not grouping, there can't be any
		 * references to non-aggregated input columns, so no problem.)
		 */
		econtext->ecxt_outertuple = firstSlot;

		/*
		 * Check the qual (HAVING clause); if the group does not match, ignore
		 * it and loop back to try to process another group.
		 */
		if (ExecQual(aggstate->ss.ps.qual, econtext, false))
		{
			/*
			 * Form and return a projection tuple using the aggregate results
			 * and the representative input tuple.	Note we do not support
			 * aggregates returning sets ...
			 */
			return ExecProject(projInfo, NULL);
		}
	}

	/* No more groups */
	return NULL;
}

/*
 * ExecAgg for hashed case: phase 1, read input and build hash table
 */
static void
agg_fill_hash_table(AggState *aggstate)
{
	PlanState  *outerPlan;
	ExprContext *tmpcontext;
	AggHashEntry entry;
	TupleTableSlot *outerslot;

	/*
	 * get state info from node
	 */
	outerPlan = outerPlanState(aggstate);
	/* tmpcontext is the per-input-tuple expression context */
	tmpcontext = aggstate->tmpcontext;

	/*
	 * Process each outer-plan tuple, and then fetch the next one, until we
	 * exhaust the outer plan.
	 */
	for (;;)
	{
		outerslot = ExecProcNode(outerPlan);
		if (TupIsNull(outerslot))
			break;
		/* set up for advance_aggregates call */
		tmpcontext->ecxt_outertuple = outerslot;

		/* Find or build hashtable entry for this tuple's group */
		entry = lookup_hash_entry(aggstate, outerslot);

		/* Advance the aggregates */
		advance_aggregates(aggstate, entry->pergroup);

		/* Reset per-input-tuple context after each tuple */
		ResetExprContext(tmpcontext);
	}

	aggstate->table_filled = true;
	/* Initialize to walk the hash table */
	ResetTupleHashIterator(aggstate->hashtable, &aggstate->hashiter);
}

/*
 * ExecAgg for hashed case: phase 2, retrieving groups from hash table
 */
static TupleTableSlot *
agg_retrieve_hash_table(AggState *aggstate)
{
	ExprContext *econtext;
	ProjectionInfo *projInfo;
	Datum	   *aggvalues;
	bool	   *aggnulls;
	AggStatePerAgg peragg;
	AggStatePerGroup pergroup;
	AggHashEntry entry;
	TupleTableSlot *firstSlot;
	int			aggno;

	/*
	 * get state info from node
	 */
	/* econtext is the per-output-tuple expression context */
	econtext = aggstate->ss.ps.ps_ExprContext;
	aggvalues = econtext->ecxt_aggvalues;
	aggnulls = econtext->ecxt_aggnulls;
	projInfo = aggstate->ss.ps.ps_ProjInfo;
	peragg = aggstate->peragg;
	firstSlot = aggstate->ss.ss_ScanTupleSlot;

	/*
	 * We loop retrieving groups until we find one satisfying
	 * aggstate->ss.ps.qual
	 */
	while (!aggstate->agg_done)
	{
		/*
		 * Find the next entry in the hash table
		 */
		entry = (AggHashEntry) ScanTupleHashTable(&aggstate->hashiter);
		if (entry == NULL)
		{
			/* No more entries in hashtable, so done */
			aggstate->agg_done = TRUE;
			return NULL;
		}

		/*
		 * Clear the per-output-tuple context for each group
		 */
		ResetExprContext(econtext);

		/*
		 * Store the copied first input tuple in the tuple table slot reserved
		 * for it, so that it can be used in ExecProject.
		 */
		ExecStoreMinimalTuple(entry->shared.firstTuple,
							  firstSlot,
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
		 * Use the representative input tuple for any references to
		 * non-aggregated input columns in the qual and tlist.
		 */
		econtext->ecxt_outertuple = firstSlot;

		/*
		 * Check the qual (HAVING clause); if the group does not match, ignore
		 * it and loop back to try to process another group.
		 */
		if (ExecQual(aggstate->ss.ps.qual, econtext, false))
		{
			/*
			 * Form and return a projection tuple using the aggregate results
			 * and the representative input tuple.	Note we do not support
			 * aggregates returning sets ...
			 */
			return ExecProject(projInfo, NULL);
		}
	}

	/* No more groups */
	return NULL;
}

/* -----------------
 * ExecInitAgg
 *
 *	Creates the run-time information for the agg node produced by the
 *	planner and initializes its outer subtree
 * -----------------
 */
AggState *
ExecInitAgg(Agg *node, EState *estate, int eflags)
{
	AggState   *aggstate;
	AggStatePerAgg peragg;
	Plan	   *outerPlan;
	ExprContext *econtext;
	int			numaggs,
				aggno;
	ListCell   *l;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	aggstate = makeNode(AggState);
	aggstate->ss.ps.plan = (Plan *) node;
	aggstate->ss.ps.state = estate;

	aggstate->aggs = NIL;
	aggstate->numaggs = 0;
	aggstate->eqfunctions = NULL;
	aggstate->hashfunctions = NULL;
	aggstate->peragg = NULL;
	aggstate->agg_done = false;
	aggstate->pergroup = NULL;
	aggstate->grp_firstTuple = NULL;
	aggstate->hashtable = NULL;

	/*
	 * Create expression contexts.	We need two, one for per-input-tuple
	 * processing and one for per-output-tuple processing.	We cheat a little
	 * by using ExecAssignExprContext() to build both.
	 */
	ExecAssignExprContext(estate, &aggstate->ss.ps);
	aggstate->tmpcontext = aggstate->ss.ps.ps_ExprContext;
	ExecAssignExprContext(estate, &aggstate->ss.ps);

	/*
	 * We also need a long-lived memory context for holding hashtable data
	 * structures and transition values.  NOTE: the details of what is stored
	 * in aggcontext and what is stored in the regular per-query memory
	 * context are driven by a simple decision: we want to reset the
	 * aggcontext in ExecReScanAgg to recover no-longer-wanted space.
	 */
	aggstate->aggcontext =
		AllocSetContextCreate(CurrentMemoryContext,
							  "AggContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

#define AGG_NSLOTS 3

	/*
	 * tuple table initialization
	 */
	ExecInitScanTupleSlot(estate, &aggstate->ss);
	ExecInitResultTupleSlot(estate, &aggstate->ss.ps);
	aggstate->hashslot = ExecInitExtraTupleSlot(estate);

	/*
	 * initialize child expressions
	 *
	 * Note: ExecInitExpr finds Aggrefs for us, and also checks that no aggs
	 * contain other agg calls in their arguments.	This would make no sense
	 * under SQL semantics anyway (and it's forbidden by the spec). Because
	 * that is true, we don't need to worry about evaluating the aggs in any
	 * particular order.
	 */
	aggstate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->plan.targetlist,
					 (PlanState *) aggstate);
	aggstate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->plan.qual,
					 (PlanState *) aggstate);

	/*
	 * initialize child nodes
	 *
	 * If we are doing a hashed aggregation then the child plan does not need
	 * to handle REWIND efficiently; see ExecReScanAgg.
	 */
	if (node->aggstrategy == AGG_HASHED)
		eflags &= ~EXEC_FLAG_REWIND;
	outerPlan = outerPlan(node);
	outerPlanState(aggstate) = ExecInitNode(outerPlan, estate, eflags);

	/*
	 * initialize source tuple type.
	 */
	ExecAssignScanTypeFromOuterPlan(&aggstate->ss);

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&aggstate->ss.ps);
	ExecAssignProjectionInfo(&aggstate->ss.ps, NULL);

	/*
	 * get the count of aggregates in targetlist and quals
	 */
	numaggs = aggstate->numaggs;
	Assert(numaggs == list_length(aggstate->aggs));
	if (numaggs <= 0)
	{
		/*
		 * This is not an error condition: we might be using the Agg node just
		 * to do hash-based grouping.  Even in the regular case,
		 * constant-expression simplification could optimize away all of the
		 * Aggrefs in the targetlist and qual.	So keep going, but force local
		 * copy of numaggs positive so that palloc()s below don't choke.
		 */
		numaggs = 1;
	}

	/*
	 * If we are grouping, precompute fmgr lookup data for inner loop. We need
	 * both equality and hashing functions to do it by hashing, but only
	 * equality if not hashing.
	 */
	if (node->numCols > 0)
	{
		if (node->aggstrategy == AGG_HASHED)
			execTuplesHashPrepare(node->numCols,
								  node->grpOperators,
								  &aggstate->eqfunctions,
								  &aggstate->hashfunctions);
		else
			aggstate->eqfunctions =
				execTuplesMatchPrepare(node->numCols,
									   node->grpOperators);
	}

	/*
	 * Set up aggregate-result storage in the output expr context, and also
	 * allocate my private per-agg working storage
	 */
	econtext = aggstate->ss.ps.ps_ExprContext;
	econtext->ecxt_aggvalues = (Datum *) palloc0(sizeof(Datum) * numaggs);
	econtext->ecxt_aggnulls = (bool *) palloc0(sizeof(bool) * numaggs);

	peragg = (AggStatePerAgg) palloc0(sizeof(AggStatePerAggData) * numaggs);
	aggstate->peragg = peragg;

	if (node->aggstrategy == AGG_HASHED)
	{
		build_hash_table(aggstate);
		aggstate->table_filled = false;
	}
	else
	{
		AggStatePerGroup pergroup;

		pergroup = (AggStatePerGroup) palloc0(sizeof(AggStatePerGroupData) * numaggs);
		aggstate->pergroup = pergroup;
	}

	/*
	 * Perform lookups of aggregate function info, and initialize the
	 * unchanging fields of the per-agg data.  We also detect duplicate
	 * aggregates (for example, "SELECT sum(x) ... HAVING sum(x) > 0"). When
	 * duplicates are detected, we only make an AggStatePerAgg struct for the
	 * first one.  The clones are simply pointed at the same result entry by
	 * giving them duplicate aggno values.
	 */
	aggno = -1;
	foreach(l, aggstate->aggs)
	{
		AggrefExprState *aggrefstate = (AggrefExprState *) lfirst(l);
		Aggref	   *aggref = (Aggref *) aggrefstate->xprstate.expr;
		AggStatePerAgg peraggstate;
		Oid			inputTypes[FUNC_MAX_ARGS];
		int			numArguments;
		HeapTuple	aggTuple;
		Form_pg_aggregate aggform;
		Oid			aggtranstype;
		AclResult	aclresult;
		Oid			transfn_oid,
					finalfn_oid;
		Expr	   *transfnexpr,
				   *finalfnexpr;
		Datum		textInitVal;
		int			i;
		ListCell   *lc;

		/* Planner should have assigned aggregate to correct level */
		Assert(aggref->agglevelsup == 0);

		/* Look for a previous duplicate aggregate */
		for (i = 0; i <= aggno; i++)
		{
			if (equal(aggref, peragg[i].aggref) &&
				!contain_volatile_functions((Node *) aggref))
				break;
		}
		if (i <= aggno)
		{
			/* Found a match to an existing entry, so just mark it */
			aggrefstate->aggno = i;
			continue;
		}

		/* Nope, so assign a new PerAgg record */
		peraggstate = &peragg[++aggno];

		/* Mark Aggref state node with assigned index in the result array */
		aggrefstate->aggno = aggno;

		/* Fill in the peraggstate data */
		peraggstate->aggrefstate = aggrefstate;
		peraggstate->aggref = aggref;
		numArguments = list_length(aggref->args);
		peraggstate->numArguments = numArguments;

		/*
		 * Get actual datatypes of the inputs.	These could be different from
		 * the agg's declared input types, when the agg accepts ANY or a
		 * polymorphic type.
		 */
		i = 0;
		foreach(lc, aggref->args)
		{
			inputTypes[i++] = exprType((Node *) lfirst(lc));
		}

		aggTuple = SearchSysCache(AGGFNOID,
								  ObjectIdGetDatum(aggref->aggfnoid),
								  0, 0, 0);
		if (!HeapTupleIsValid(aggTuple))
			elog(ERROR, "cache lookup failed for aggregate %u",
				 aggref->aggfnoid);
		aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);

		/* Check permission to call aggregate function */
		aclresult = pg_proc_aclcheck(aggref->aggfnoid, GetUserId(),
									 ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_PROC,
						   get_func_name(aggref->aggfnoid));

		peraggstate->transfn_oid = transfn_oid = aggform->aggtransfn;
		peraggstate->finalfn_oid = finalfn_oid = aggform->aggfinalfn;

		/* Check that aggregate owner has permission to call component fns */
		{
			HeapTuple	procTuple;
			Oid			aggOwner;

			procTuple = SearchSysCache(PROCOID,
									   ObjectIdGetDatum(aggref->aggfnoid),
									   0, 0, 0);
			if (!HeapTupleIsValid(procTuple))
				elog(ERROR, "cache lookup failed for function %u",
					 aggref->aggfnoid);
			aggOwner = ((Form_pg_proc) GETSTRUCT(procTuple))->proowner;
			ReleaseSysCache(procTuple);

			aclresult = pg_proc_aclcheck(transfn_oid, aggOwner,
										 ACL_EXECUTE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, ACL_KIND_PROC,
							   get_func_name(transfn_oid));
			if (OidIsValid(finalfn_oid))
			{
				aclresult = pg_proc_aclcheck(finalfn_oid, aggOwner,
											 ACL_EXECUTE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, ACL_KIND_PROC,
								   get_func_name(finalfn_oid));
			}
		}

		/* resolve actual type of transition state, if polymorphic */
		aggtranstype = aggform->aggtranstype;
		if (IsPolymorphicType(aggtranstype))
		{
			/* have to fetch the agg's declared input types... */
			Oid		   *declaredArgTypes;
			int			agg_nargs;

			(void) get_func_signature(aggref->aggfnoid,
									  &declaredArgTypes, &agg_nargs);
			Assert(agg_nargs == numArguments);
			aggtranstype = enforce_generic_type_consistency(inputTypes,
															declaredArgTypes,
															agg_nargs,
															aggtranstype,
															false);
			pfree(declaredArgTypes);
		}

		/* build expression trees using actual argument & result types */
		build_aggregate_fnexprs(inputTypes,
								numArguments,
								aggtranstype,
								aggref->aggtype,
								transfn_oid,
								finalfn_oid,
								&transfnexpr,
								&finalfnexpr);

		fmgr_info(transfn_oid, &peraggstate->transfn);
		peraggstate->transfn.fn_expr = (Node *) transfnexpr;

		if (OidIsValid(finalfn_oid))
		{
			fmgr_info(finalfn_oid, &peraggstate->finalfn);
			peraggstate->finalfn.fn_expr = (Node *) finalfnexpr;
		}

		get_typlenbyval(aggref->aggtype,
						&peraggstate->resulttypeLen,
						&peraggstate->resulttypeByVal);
		get_typlenbyval(aggtranstype,
						&peraggstate->transtypeLen,
						&peraggstate->transtypeByVal);

		/*
		 * initval is potentially null, so don't try to access it as a struct
		 * field. Must do it the hard way with SysCacheGetAttr.
		 */
		textInitVal = SysCacheGetAttr(AGGFNOID, aggTuple,
									  Anum_pg_aggregate_agginitval,
									  &peraggstate->initValueIsNull);

		if (peraggstate->initValueIsNull)
			peraggstate->initValue = (Datum) 0;
		else
			peraggstate->initValue = GetAggInitVal(textInitVal,
												   aggtranstype);

		/*
		 * If the transfn is strict and the initval is NULL, make sure input
		 * type and transtype are the same (or at least binary-compatible), so
		 * that it's OK to use the first input value as the initial
		 * transValue.	This should have been checked at agg definition time,
		 * but just in case...
		 */
		if (peraggstate->transfn.fn_strict && peraggstate->initValueIsNull)
		{
			if (numArguments < 1 ||
				!IsBinaryCoercible(inputTypes[0], aggtranstype))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("aggregate %u needs to have compatible input type and transition type",
								aggref->aggfnoid)));
		}

		if (aggref->aggdistinct)
		{
			Oid			eq_function;

			/* We don't implement DISTINCT aggs in the HASHED case */
			Assert(node->aggstrategy != AGG_HASHED);

			/*
			 * We don't currently implement DISTINCT aggs for aggs having more
			 * than one argument.  This isn't required for anything in the SQL
			 * spec, but really it ought to be implemented for
			 * feature-completeness.  FIXME someday.
			 */
			if (numArguments != 1)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("DISTINCT is supported only for single-argument aggregates")));

			peraggstate->inputType = inputTypes[0];
			get_typlenbyval(inputTypes[0],
							&peraggstate->inputtypeLen,
							&peraggstate->inputtypeByVal);

			/*
			 * Look up the sorting and comparison operators to use.  XXX it's
			 * pretty bletcherous to be making this sort of semantic decision
			 * in the executor.  Probably the parser should decide this and
			 * record it in the Aggref node ... or at latest, do it in the
			 * planner.
			 */
			eq_function = equality_oper_funcid(inputTypes[0]);
			fmgr_info(eq_function, &(peraggstate->equalfn));
			peraggstate->sortOperator = ordering_oper_opid(inputTypes[0]);
			peraggstate->sortstate = NULL;
		}

		ReleaseSysCache(aggTuple);
	}

	/* Update numaggs to match number of unique aggregates found */
	aggstate->numaggs = aggno + 1;

	return aggstate;
}

static Datum
GetAggInitVal(Datum textInitVal, Oid transtype)
{
	Oid			typinput,
				typioparam;
	char	   *strInitVal;
	Datum		initVal;

	getTypeInputInfo(transtype, &typinput, &typioparam);
	strInitVal = DatumGetCString(DirectFunctionCall1(textout, textInitVal));
	initVal = OidInputFunctionCall(typinput, strInitVal,
								   typioparam, -1);
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
ExecEndAgg(AggState *node)
{
	PlanState  *outerPlan;
	int			aggno;

	/* Make sure we have closed any open tuplesorts */
	for (aggno = 0; aggno < node->numaggs; aggno++)
	{
		AggStatePerAgg peraggstate = &node->peragg[aggno];

		if (peraggstate->sortstate)
			tuplesort_end(peraggstate->sortstate);
	}

	/*
	 * Free both the expr contexts.
	 */
	ExecFreeExprContext(&node->ss.ps);
	node->ss.ps.ps_ExprContext = node->tmpcontext;
	ExecFreeExprContext(&node->ss.ps);

	/* clean up tuple table */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	MemoryContextDelete(node->aggcontext);

	outerPlan = outerPlanState(node);
	ExecEndNode(outerPlan);
}

void
ExecReScanAgg(AggState *node, ExprContext *exprCtxt)
{
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	int			aggno;

	node->agg_done = false;

	if (((Agg *) node->ss.ps.plan)->aggstrategy == AGG_HASHED)
	{
		/*
		 * In the hashed case, if we haven't yet built the hash table then we
		 * can just return; nothing done yet, so nothing to undo. If subnode's
		 * chgParam is not NULL then it will be re-scanned by ExecProcNode,
		 * else no reason to re-scan it at all.
		 */
		if (!node->table_filled)
			return;

		/*
		 * If we do have the hash table and the subplan does not have any
		 * parameter changes, then we can just rescan the existing hash table;
		 * no need to build it again.
		 */
		if (((PlanState *) node)->lefttree->chgParam == NULL)
		{
			ResetTupleHashIterator(node->hashtable, &node->hashiter);
			return;
		}
	}

	/* Make sure we have closed any open tuplesorts */
	for (aggno = 0; aggno < node->numaggs; aggno++)
	{
		AggStatePerAgg peraggstate = &node->peragg[aggno];

		if (peraggstate->sortstate)
			tuplesort_end(peraggstate->sortstate);
		peraggstate->sortstate = NULL;
	}

	/* Release first tuple of group, if we have made a copy */
	if (node->grp_firstTuple != NULL)
	{
		heap_freetuple(node->grp_firstTuple);
		node->grp_firstTuple = NULL;
	}

	/* Forget current agg values */
	MemSet(econtext->ecxt_aggvalues, 0, sizeof(Datum) * node->numaggs);
	MemSet(econtext->ecxt_aggnulls, 0, sizeof(bool) * node->numaggs);

	/*
	 * Release all temp storage. Note that with AGG_HASHED, the hash table is
	 * allocated in a sub-context of the aggcontext. We're going to rebuild
	 * the hash table from scratch, so we need to use
	 * MemoryContextResetAndDeleteChildren() to avoid leaking the old hash
	 * table's memory context header.
	 */
	MemoryContextResetAndDeleteChildren(node->aggcontext);

	if (((Agg *) node->ss.ps.plan)->aggstrategy == AGG_HASHED)
	{
		/* Rebuild an empty hash table */
		build_hash_table(node);
		node->table_filled = false;
	}
	else
	{
		/*
		 * Reset the per-group state (in particular, mark transvalues null)
		 */
		MemSet(node->pergroup, 0,
			   sizeof(AggStatePerGroupData) * node->numaggs);
	}

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((PlanState *) node)->lefttree->chgParam == NULL)
		ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
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
	elog(ERROR, "aggregate function %u called as normal function",
		 fcinfo->flinfo->fn_oid);
	return (Datum) 0;			/* keep compiler quiet */
}
