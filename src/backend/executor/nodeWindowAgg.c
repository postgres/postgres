/*-------------------------------------------------------------------------
 *
 * nodeWindowAgg.c
 *	  routines to handle WindowAgg nodes.
 *
 * A WindowAgg node evaluates "window functions" across suitable partitions
 * of the input tuple set.  Any one WindowAgg works for just a single window
 * specification, though it can evaluate multiple window functions sharing
 * identical window specifications.  The input tuples are required to be
 * delivered in sorted order, with the PARTITION BY columns (if any) as
 * major sort keys and the ORDER BY columns (if any) as minor sort keys.
 * (The planner generates a stack of WindowAggs with intervening Sort nodes
 * as needed, if a query involves more than one window specification.)
 *
 * Since window functions can require access to any or all of the rows in
 * the current partition, we accumulate rows of the partition into a
 * tuplestore.  The window functions are called using the WindowObject API
 * so that they can access those rows as needed.
 *
 * We also support using plain aggregate functions as window functions.
 * For these, the regular Agg-node environment is emulated for each partition.
 * As required by the SQL spec, the output represents the value of the
 * aggregate function over all rows in the current row's window frame.
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeWindowAgg.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "executor/executor.h"
#include "executor/nodeWindowAgg.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "windowapi.h"

/*
 * All the window function APIs are called with this object, which is passed
 * to window functions as fcinfo->context.
 */
typedef struct WindowObjectData
{
	NodeTag		type;
	WindowAggState *winstate;	/* parent WindowAggState */
	List	   *argstates;		/* ExprState trees for fn's arguments */
	void	   *localmem;		/* WinGetPartitionLocalMemory's chunk */
	int			markptr;		/* tuplestore mark pointer for this fn */
	int			readptr;		/* tuplestore read pointer for this fn */
	int64		markpos;		/* row that markptr is positioned on */
	int64		seekpos;		/* row that readptr is positioned on */
} WindowObjectData;

/*
 * We have one WindowStatePerFunc struct for each window function and
 * window aggregate handled by this node.
 */
typedef struct WindowStatePerFuncData
{
	/* Links to WindowFunc expr and state nodes this working state is for */
	WindowFuncExprState *wfuncstate;
	WindowFunc *wfunc;

	int			numArguments;	/* number of arguments */

	FmgrInfo	flinfo;			/* fmgr lookup data for window function */

	Oid			winCollation;	/* collation derived for window function */

	/*
	 * We need the len and byval info for the result of each function in order
	 * to know how to copy/delete values.
	 */
	int16		resulttypeLen;
	bool		resulttypeByVal;

	bool		plain_agg;		/* is it just a plain aggregate function? */
	int			aggno;			/* if so, index of its PerAggData */

	WindowObject winobj;		/* object used in window function API */
}	WindowStatePerFuncData;

/*
 * For plain aggregate window functions, we also have one of these.
 */
typedef struct WindowStatePerAggData
{
	/* Oids of transition functions */
	Oid			transfn_oid;
	Oid			invtransfn_oid; /* may be InvalidOid */
	Oid			finalfn_oid;	/* may be InvalidOid */

	/*
	 * fmgr lookup data for transition functions --- only valid when
	 * corresponding oid is not InvalidOid.  Note in particular that fn_strict
	 * flags are kept here.
	 */
	FmgrInfo	transfn;
	FmgrInfo	invtransfn;
	FmgrInfo	finalfn;

	int			numFinalArgs;	/* number of arguments to pass to finalfn */

	/*
	 * initial value from pg_aggregate entry
	 */
	Datum		initValue;
	bool		initValueIsNull;

	/*
	 * cached value for current frame boundaries
	 */
	Datum		resultValue;
	bool		resultValueIsNull;

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

	int			wfuncno;		/* index of associated PerFuncData */

	/* Context holding transition value and possibly other subsidiary data */
	MemoryContext aggcontext;	/* may be private, or winstate->aggcontext */

	/* Current transition value */
	Datum		transValue;		/* current transition value */
	bool		transValueIsNull;

	int64		transValueCount;	/* number of currently-aggregated rows */

	/* Data local to eval_windowaggregates() */
	bool		restart;		/* need to restart this agg in this cycle? */
} WindowStatePerAggData;

static void initialize_windowaggregate(WindowAggState *winstate,
						   WindowStatePerFunc perfuncstate,
						   WindowStatePerAgg peraggstate);
static void advance_windowaggregate(WindowAggState *winstate,
						WindowStatePerFunc perfuncstate,
						WindowStatePerAgg peraggstate);
static bool advance_windowaggregate_base(WindowAggState *winstate,
							 WindowStatePerFunc perfuncstate,
							 WindowStatePerAgg peraggstate);
static void finalize_windowaggregate(WindowAggState *winstate,
						 WindowStatePerFunc perfuncstate,
						 WindowStatePerAgg peraggstate,
						 Datum *result, bool *isnull);

static void eval_windowaggregates(WindowAggState *winstate);
static void eval_windowfunction(WindowAggState *winstate,
					WindowStatePerFunc perfuncstate,
					Datum *result, bool *isnull);

static void begin_partition(WindowAggState *winstate);
static void spool_tuples(WindowAggState *winstate, int64 pos);
static void release_partition(WindowAggState *winstate);

static bool row_is_in_frame(WindowAggState *winstate, int64 pos,
				TupleTableSlot *slot);
static void update_frameheadpos(WindowObject winobj, TupleTableSlot *slot);
static void update_frametailpos(WindowObject winobj, TupleTableSlot *slot);

static WindowStatePerAggData *initialize_peragg(WindowAggState *winstate,
				  WindowFunc *wfunc,
				  WindowStatePerAgg peraggstate);
static Datum GetAggInitVal(Datum textInitVal, Oid transtype);

static bool are_peers(WindowAggState *winstate, TupleTableSlot *slot1,
		  TupleTableSlot *slot2);
static bool window_gettupleslot(WindowObject winobj, int64 pos,
					TupleTableSlot *slot);


/*
 * initialize_windowaggregate
 * parallel to initialize_aggregates in nodeAgg.c
 */
static void
initialize_windowaggregate(WindowAggState *winstate,
						   WindowStatePerFunc perfuncstate,
						   WindowStatePerAgg peraggstate)
{
	MemoryContext oldContext;

	/*
	 * If we're using a private aggcontext, we may reset it here.  But if the
	 * context is shared, we don't know which other aggregates may still need
	 * it, so we must leave it to the caller to reset at an appropriate time.
	 */
	if (peraggstate->aggcontext != winstate->aggcontext)
		MemoryContextResetAndDeleteChildren(peraggstate->aggcontext);

	if (peraggstate->initValueIsNull)
		peraggstate->transValue = peraggstate->initValue;
	else
	{
		oldContext = MemoryContextSwitchTo(peraggstate->aggcontext);
		peraggstate->transValue = datumCopy(peraggstate->initValue,
											peraggstate->transtypeByVal,
											peraggstate->transtypeLen);
		MemoryContextSwitchTo(oldContext);
	}
	peraggstate->transValueIsNull = peraggstate->initValueIsNull;
	peraggstate->transValueCount = 0;
	peraggstate->resultValue = (Datum) 0;
	peraggstate->resultValueIsNull = true;
}

/*
 * advance_windowaggregate
 * parallel to advance_aggregates in nodeAgg.c
 */
static void
advance_windowaggregate(WindowAggState *winstate,
						WindowStatePerFunc perfuncstate,
						WindowStatePerAgg peraggstate)
{
	WindowFuncExprState *wfuncstate = perfuncstate->wfuncstate;
	int			numArguments = perfuncstate->numArguments;
	FunctionCallInfoData fcinfodata;
	FunctionCallInfo fcinfo = &fcinfodata;
	Datum		newVal;
	ListCell   *arg;
	int			i;
	MemoryContext oldContext;
	ExprContext *econtext = winstate->tmpcontext;
	ExprState  *filter = wfuncstate->aggfilter;

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/* Skip anything FILTERed out */
	if (filter)
	{
		bool		isnull;
		Datum		res = ExecEvalExpr(filter, econtext, &isnull, NULL);

		if (isnull || !DatumGetBool(res))
		{
			MemoryContextSwitchTo(oldContext);
			return;
		}
	}

	/* We start from 1, since the 0th arg will be the transition value */
	i = 1;
	foreach(arg, wfuncstate->args)
	{
		ExprState  *argstate = (ExprState *) lfirst(arg);

		fcinfo->arg[i] = ExecEvalExpr(argstate, econtext,
									  &fcinfo->argnull[i], NULL);
		i++;
	}

	if (peraggstate->transfn.fn_strict)
	{
		/*
		 * For a strict transfn, nothing happens when there's a NULL input; we
		 * just keep the prior transValue.  Note transValueCount doesn't
		 * change either.
		 */
		for (i = 1; i <= numArguments; i++)
		{
			if (fcinfo->argnull[i])
			{
				MemoryContextSwitchTo(oldContext);
				return;
			}
		}

		/*
		 * For strict transition functions with initial value NULL we use the
		 * first non-NULL input as the initial state.  (We already checked
		 * that the agg's input type is binary-compatible with its transtype,
		 * so straight copy here is OK.)
		 *
		 * We must copy the datum into aggcontext if it is pass-by-ref.  We do
		 * not need to pfree the old transValue, since it's NULL.
		 */
		if (peraggstate->transValueCount == 0 && peraggstate->transValueIsNull)
		{
			MemoryContextSwitchTo(peraggstate->aggcontext);
			peraggstate->transValue = datumCopy(fcinfo->arg[1],
												peraggstate->transtypeByVal,
												peraggstate->transtypeLen);
			peraggstate->transValueIsNull = false;
			peraggstate->transValueCount = 1;
			MemoryContextSwitchTo(oldContext);
			return;
		}

		if (peraggstate->transValueIsNull)
		{
			/*
			 * Don't call a strict function with NULL inputs.  Note it is
			 * possible to get here despite the above tests, if the transfn is
			 * strict *and* returned a NULL on a prior cycle.  If that happens
			 * we will propagate the NULL all the way to the end.  That can
			 * only happen if there's no inverse transition function, though,
			 * since we disallow transitions back to NULL when there is one.
			 */
			MemoryContextSwitchTo(oldContext);
			Assert(!OidIsValid(peraggstate->invtransfn_oid));
			return;
		}
	}

	/*
	 * OK to call the transition function.  Set winstate->curaggcontext while
	 * calling it, for possible use by AggCheckCallContext.
	 */
	InitFunctionCallInfoData(*fcinfo, &(peraggstate->transfn),
							 numArguments + 1,
							 perfuncstate->winCollation,
							 (void *) winstate, NULL);
	fcinfo->arg[0] = peraggstate->transValue;
	fcinfo->argnull[0] = peraggstate->transValueIsNull;
	winstate->curaggcontext = peraggstate->aggcontext;
	newVal = FunctionCallInvoke(fcinfo);
	winstate->curaggcontext = NULL;

	/*
	 * Moving-aggregate transition functions must not return null, see
	 * advance_windowaggregate_base().
	 */
	if (fcinfo->isnull && OidIsValid(peraggstate->invtransfn_oid))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		errmsg("moving-aggregate transition function must not return null")));

	/*
	 * We must track the number of rows included in transValue, since to
	 * remove the last input, advance_windowaggregate_base() musn't call the
	 * inverse transition function, but simply reset transValue back to its
	 * initial value.
	 */
	peraggstate->transValueCount++;

	/*
	 * If pass-by-ref datatype, must copy the new value into aggcontext and
	 * pfree the prior transValue.  But if transfn returned a pointer to its
	 * first input, we don't need to do anything.
	 */
	if (!peraggstate->transtypeByVal &&
		DatumGetPointer(newVal) != DatumGetPointer(peraggstate->transValue))
	{
		if (!fcinfo->isnull)
		{
			MemoryContextSwitchTo(peraggstate->aggcontext);
			newVal = datumCopy(newVal,
							   peraggstate->transtypeByVal,
							   peraggstate->transtypeLen);
		}
		if (!peraggstate->transValueIsNull)
			pfree(DatumGetPointer(peraggstate->transValue));
	}

	MemoryContextSwitchTo(oldContext);
	peraggstate->transValue = newVal;
	peraggstate->transValueIsNull = fcinfo->isnull;
}

/*
 * advance_windowaggregate_base
 * Remove the oldest tuple from an aggregation.
 *
 * This is very much like advance_windowaggregate, except that we will call
 * the inverse transition function (which caller must have checked is
 * available).
 *
 * Returns true if we successfully removed the current row from this
 * aggregate, false if not (in the latter case, caller is responsible
 * for cleaning up by restarting the aggregation).
 */
static bool
advance_windowaggregate_base(WindowAggState *winstate,
							 WindowStatePerFunc perfuncstate,
							 WindowStatePerAgg peraggstate)
{
	WindowFuncExprState *wfuncstate = perfuncstate->wfuncstate;
	int			numArguments = perfuncstate->numArguments;
	FunctionCallInfoData fcinfodata;
	FunctionCallInfo fcinfo = &fcinfodata;
	Datum		newVal;
	ListCell   *arg;
	int			i;
	MemoryContext oldContext;
	ExprContext *econtext = winstate->tmpcontext;
	ExprState  *filter = wfuncstate->aggfilter;

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/* Skip anything FILTERed out */
	if (filter)
	{
		bool		isnull;
		Datum		res = ExecEvalExpr(filter, econtext, &isnull, NULL);

		if (isnull || !DatumGetBool(res))
		{
			MemoryContextSwitchTo(oldContext);
			return true;
		}
	}

	/* We start from 1, since the 0th arg will be the transition value */
	i = 1;
	foreach(arg, wfuncstate->args)
	{
		ExprState  *argstate = (ExprState *) lfirst(arg);

		fcinfo->arg[i] = ExecEvalExpr(argstate, econtext,
									  &fcinfo->argnull[i], NULL);
		i++;
	}

	if (peraggstate->invtransfn.fn_strict)
	{
		/*
		 * For a strict (inv)transfn, nothing happens when there's a NULL
		 * input; we just keep the prior transValue.  Note transValueCount
		 * doesn't change either.
		 */
		for (i = 1; i <= numArguments; i++)
		{
			if (fcinfo->argnull[i])
			{
				MemoryContextSwitchTo(oldContext);
				return true;
			}
		}
	}

	/* There should still be an added but not yet removed value */
	Assert(peraggstate->transValueCount > 0);

	/*
	 * In moving-aggregate mode, the state must never be NULL, except possibly
	 * before any rows have been aggregated (which is surely not the case at
	 * this point).  This restriction allows us to interpret a NULL result
	 * from the inverse function as meaning "sorry, can't do an inverse
	 * transition in this case".  We already checked this in
	 * advance_windowaggregate, but just for safety, check again.
	 */
	if (peraggstate->transValueIsNull)
		elog(ERROR, "aggregate transition value is NULL before inverse transition");

	/*
	 * We mustn't use the inverse transition function to remove the last
	 * input.  Doing so would yield a non-NULL state, whereas we should be in
	 * the initial state afterwards which may very well be NULL.  So instead,
	 * we simply re-initialize the aggregate in this case.
	 */
	if (peraggstate->transValueCount == 1)
	{
		MemoryContextSwitchTo(oldContext);
		initialize_windowaggregate(winstate,
								   &winstate->perfunc[peraggstate->wfuncno],
								   peraggstate);
		return true;
	}

	/*
	 * OK to call the inverse transition function.  Set
	 * winstate->curaggcontext while calling it, for possible use by
	 * AggCheckCallContext.
	 */
	InitFunctionCallInfoData(*fcinfo, &(peraggstate->invtransfn),
							 numArguments + 1,
							 perfuncstate->winCollation,
							 (void *) winstate, NULL);
	fcinfo->arg[0] = peraggstate->transValue;
	fcinfo->argnull[0] = peraggstate->transValueIsNull;
	winstate->curaggcontext = peraggstate->aggcontext;
	newVal = FunctionCallInvoke(fcinfo);
	winstate->curaggcontext = NULL;

	/*
	 * If the function returns NULL, report failure, forcing a restart.
	 */
	if (fcinfo->isnull)
	{
		MemoryContextSwitchTo(oldContext);
		return false;
	}

	/* Update number of rows included in transValue */
	peraggstate->transValueCount--;

	/*
	 * If pass-by-ref datatype, must copy the new value into aggcontext and
	 * pfree the prior transValue.  But if invtransfn returned a pointer to
	 * its first input, we don't need to do anything.
	 *
	 * Note: the checks for null values here will never fire, but it seems
	 * best to have this stanza look just like advance_windowaggregate.
	 */
	if (!peraggstate->transtypeByVal &&
		DatumGetPointer(newVal) != DatumGetPointer(peraggstate->transValue))
	{
		if (!fcinfo->isnull)
		{
			MemoryContextSwitchTo(peraggstate->aggcontext);
			newVal = datumCopy(newVal,
							   peraggstate->transtypeByVal,
							   peraggstate->transtypeLen);
		}
		if (!peraggstate->transValueIsNull)
			pfree(DatumGetPointer(peraggstate->transValue));
	}

	MemoryContextSwitchTo(oldContext);
	peraggstate->transValue = newVal;
	peraggstate->transValueIsNull = fcinfo->isnull;

	return true;
}

/*
 * finalize_windowaggregate
 * parallel to finalize_aggregate in nodeAgg.c
 */
static void
finalize_windowaggregate(WindowAggState *winstate,
						 WindowStatePerFunc perfuncstate,
						 WindowStatePerAgg peraggstate,
						 Datum *result, bool *isnull)
{
	MemoryContext oldContext;

	oldContext = MemoryContextSwitchTo(winstate->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

	/*
	 * Apply the agg's finalfn if one is provided, else return transValue.
	 */
	if (OidIsValid(peraggstate->finalfn_oid))
	{
		int			numFinalArgs = peraggstate->numFinalArgs;
		FunctionCallInfoData fcinfo;
		bool		anynull;
		int			i;

		InitFunctionCallInfoData(fcinfo, &(peraggstate->finalfn),
								 numFinalArgs,
								 perfuncstate->winCollation,
								 (void *) winstate, NULL);
		fcinfo.arg[0] = peraggstate->transValue;
		fcinfo.argnull[0] = peraggstate->transValueIsNull;
		anynull = peraggstate->transValueIsNull;

		/* Fill any remaining argument positions with nulls */
		for (i = 1; i < numFinalArgs; i++)
		{
			fcinfo.arg[i] = (Datum) 0;
			fcinfo.argnull[i] = true;
			anynull = true;
		}

		if (fcinfo.flinfo->fn_strict && anynull)
		{
			/* don't call a strict function with NULL inputs */
			*result = (Datum) 0;
			*isnull = true;
		}
		else
		{
			winstate->curaggcontext = peraggstate->aggcontext;
			*result = FunctionCallInvoke(&fcinfo);
			winstate->curaggcontext = NULL;
			*isnull = fcinfo.isnull;
		}
	}
	else
	{
		*result = peraggstate->transValue;
		*isnull = peraggstate->transValueIsNull;
	}

	/*
	 * If result is pass-by-ref, make sure it is in the right context.
	 */
	if (!peraggstate->resulttypeByVal && !*isnull &&
		!MemoryContextContains(CurrentMemoryContext,
							   DatumGetPointer(*result)))
		*result = datumCopy(*result,
							peraggstate->resulttypeByVal,
							peraggstate->resulttypeLen);
	MemoryContextSwitchTo(oldContext);
}

/*
 * eval_windowaggregates
 * evaluate plain aggregates being used as window functions
 *
 * This differs from nodeAgg.c in two ways.  First, if the window's frame
 * start position moves, we use the inverse transition function (if it exists)
 * to remove rows from the transition value.  And second, we expect to be
 * able to call aggregate final functions repeatedly after aggregating more
 * data onto the same transition value.  This is not a behavior required by
 * nodeAgg.c.
 */
static void
eval_windowaggregates(WindowAggState *winstate)
{
	WindowStatePerAgg peraggstate;
	int			wfuncno,
				numaggs,
				numaggs_restart,
				i;
	int64		aggregatedupto_nonrestarted;
	MemoryContext oldContext;
	ExprContext *econtext;
	WindowObject agg_winobj;
	TupleTableSlot *agg_row_slot;
	TupleTableSlot *temp_slot;

	numaggs = winstate->numaggs;
	if (numaggs == 0)
		return;					/* nothing to do */

	/* final output execution is in ps_ExprContext */
	econtext = winstate->ss.ps.ps_ExprContext;
	agg_winobj = winstate->agg_winobj;
	agg_row_slot = winstate->agg_row_slot;
	temp_slot = winstate->temp_slot_1;

	/*
	 * Currently, we support only a subset of the SQL-standard window framing
	 * rules.
	 *
	 * If the frame start is UNBOUNDED_PRECEDING, the window frame consists of
	 * a contiguous group of rows extending forward from the start of the
	 * partition, and rows only enter the frame, never exit it, as the current
	 * row advances forward.  This makes it possible to use an incremental
	 * strategy for evaluating aggregates: we run the transition function for
	 * each row added to the frame, and run the final function whenever we
	 * need the current aggregate value.  This is considerably more efficient
	 * than the naive approach of re-running the entire aggregate calculation
	 * for each current row.  It does assume that the final function doesn't
	 * damage the running transition value, but we have the same assumption in
	 * nodeAgg.c too (when it rescans an existing hash table).
	 *
	 * If the frame start does sometimes move, we can still optimize as above
	 * whenever successive rows share the same frame head, but if the frame
	 * head moves beyond the previous head we try to remove those rows using
	 * the aggregate's inverse transition function.  This function restores
	 * the aggregate's current state to what it would be if the removed row
	 * had never been aggregated in the first place.  Inverse transition
	 * functions may optionally return NULL, indicating that the function was
	 * unable to remove the tuple from aggregation.  If this happens, or if
	 * the aggregate doesn't have an inverse transition function at all, we
	 * must perform the aggregation all over again for all tuples within the
	 * new frame boundaries.
	 *
	 * In many common cases, multiple rows share the same frame and hence the
	 * same aggregate value. (In particular, if there's no ORDER BY in a RANGE
	 * window, then all rows are peers and so they all have window frame equal
	 * to the whole partition.)  We optimize such cases by calculating the
	 * aggregate value once when we reach the first row of a peer group, and
	 * then returning the saved value for all subsequent rows.
	 *
	 * 'aggregatedupto' keeps track of the first row that has not yet been
	 * accumulated into the aggregate transition values.  Whenever we start a
	 * new peer group, we accumulate forward to the end of the peer group.
	 */

	/*
	 * First, update the frame head position.
	 *
	 * The frame head should never move backwards, and the code below wouldn't
	 * cope if it did, so for safety we complain if it does.
	 */
	update_frameheadpos(agg_winobj, temp_slot);
	if (winstate->frameheadpos < winstate->aggregatedbase)
		elog(ERROR, "window frame head moved backward");

	/*
	 * If the frame didn't change compared to the previous row, we can re-use
	 * the result values that were previously saved at the bottom of this
	 * function.  Since we don't know the current frame's end yet, this is not
	 * possible to check for fully.  But if the frame end mode is UNBOUNDED
	 * FOLLOWING or CURRENT ROW, and the current row lies within the previous
	 * row's frame, then the two frames' ends must coincide.  Note that on the
	 * first row aggregatedbase == aggregatedupto, meaning this test must
	 * fail, so we don't need to check the "there was no previous row" case
	 * explicitly here.
	 */
	if (winstate->aggregatedbase == winstate->frameheadpos &&
		(winstate->frameOptions & (FRAMEOPTION_END_UNBOUNDED_FOLLOWING |
								   FRAMEOPTION_END_CURRENT_ROW)) &&
		winstate->aggregatedbase <= winstate->currentpos &&
		winstate->aggregatedupto > winstate->currentpos)
	{
		for (i = 0; i < numaggs; i++)
		{
			peraggstate = &winstate->peragg[i];
			wfuncno = peraggstate->wfuncno;
			econtext->ecxt_aggvalues[wfuncno] = peraggstate->resultValue;
			econtext->ecxt_aggnulls[wfuncno] = peraggstate->resultValueIsNull;
		}
		return;
	}

	/*----------
	 * Initialize restart flags.
	 *
	 * We restart the aggregation:
	 *	 - if we're processing the first row in the partition, or
	 *	 - if the frame's head moved and we cannot use an inverse
	 *	   transition function, or
	 *	 - if the new frame doesn't overlap the old one
	 *
	 * Note that we don't strictly need to restart in the last case, but if
	 * we're going to remove all rows from the aggregation anyway, a restart
	 * surely is faster.
	 *----------
	 */
	numaggs_restart = 0;
	for (i = 0; i < numaggs; i++)
	{
		peraggstate = &winstate->peragg[i];
		if (winstate->currentpos == 0 ||
			(winstate->aggregatedbase != winstate->frameheadpos &&
			 !OidIsValid(peraggstate->invtransfn_oid)) ||
			winstate->aggregatedupto <= winstate->frameheadpos)
		{
			peraggstate->restart = true;
			numaggs_restart++;
		}
		else
			peraggstate->restart = false;
	}

	/*
	 * If we have any possibly-moving aggregates, attempt to advance
	 * aggregatedbase to match the frame's head by removing input rows that
	 * fell off the top of the frame from the aggregations.  This can fail,
	 * i.e. advance_windowaggregate_base() can return false, in which case
	 * we'll restart that aggregate below.
	 */
	while (numaggs_restart < numaggs &&
		   winstate->aggregatedbase < winstate->frameheadpos)
	{
		/*
		 * Fetch the next tuple of those being removed. This should never fail
		 * as we should have been here before.
		 */
		if (!window_gettupleslot(agg_winobj, winstate->aggregatedbase,
								 temp_slot))
			elog(ERROR, "could not re-fetch previously fetched frame row");

		/* Set tuple context for evaluation of aggregate arguments */
		winstate->tmpcontext->ecxt_outertuple = temp_slot;

		/*
		 * Perform the inverse transition for each aggregate function in the
		 * window, unless it has already been marked as needing a restart.
		 */
		for (i = 0; i < numaggs; i++)
		{
			bool		ok;

			peraggstate = &winstate->peragg[i];
			if (peraggstate->restart)
				continue;

			wfuncno = peraggstate->wfuncno;
			ok = advance_windowaggregate_base(winstate,
											  &winstate->perfunc[wfuncno],
											  peraggstate);
			if (!ok)
			{
				/* Inverse transition function has failed, must restart */
				peraggstate->restart = true;
				numaggs_restart++;
			}
		}

		/* Reset per-input-tuple context after each tuple */
		ResetExprContext(winstate->tmpcontext);

		/* And advance the aggregated-row state */
		winstate->aggregatedbase++;
		ExecClearTuple(temp_slot);
	}

	/*
	 * If we successfully advanced the base rows of all the aggregates,
	 * aggregatedbase now equals frameheadpos; but if we failed for any, we
	 * must forcibly update aggregatedbase.
	 */
	winstate->aggregatedbase = winstate->frameheadpos;

	/*
	 * If we created a mark pointer for aggregates, keep it pushed up to frame
	 * head, so that tuplestore can discard unnecessary rows.
	 */
	if (agg_winobj->markptr >= 0)
		WinSetMarkPosition(agg_winobj, winstate->frameheadpos);

	/*
	 * Now restart the aggregates that require it.
	 *
	 * We assume that aggregates using the shared context always restart if
	 * *any* aggregate restarts, and we may thus clean up the shared
	 * aggcontext if that is the case.  Private aggcontexts are reset by
	 * initialize_windowaggregate() if their owning aggregate restarts. If we
	 * aren't restarting an aggregate, we need to free any previously saved
	 * result for it, else we'll leak memory.
	 */
	if (numaggs_restart > 0)
		MemoryContextResetAndDeleteChildren(winstate->aggcontext);
	for (i = 0; i < numaggs; i++)
	{
		peraggstate = &winstate->peragg[i];

		/* Aggregates using the shared ctx must restart if *any* agg does */
		Assert(peraggstate->aggcontext != winstate->aggcontext ||
			   numaggs_restart == 0 ||
			   peraggstate->restart);

		if (peraggstate->restart)
		{
			wfuncno = peraggstate->wfuncno;
			initialize_windowaggregate(winstate,
									   &winstate->perfunc[wfuncno],
									   peraggstate);
		}
		else if (!peraggstate->resultValueIsNull)
		{
			if (!peraggstate->resulttypeByVal)
				pfree(DatumGetPointer(peraggstate->resultValue));
			peraggstate->resultValue = (Datum) 0;
			peraggstate->resultValueIsNull = true;
		}
	}

	/*
	 * Non-restarted aggregates now contain the rows between aggregatedbase
	 * (i.e., frameheadpos) and aggregatedupto, while restarted aggregates
	 * contain no rows.  If there are any restarted aggregates, we must thus
	 * begin aggregating anew at frameheadpos, otherwise we may simply
	 * continue at aggregatedupto.  We must remember the old value of
	 * aggregatedupto to know how long to skip advancing non-restarted
	 * aggregates.  If we modify aggregatedupto, we must also clear
	 * agg_row_slot, per the loop invariant below.
	 */
	aggregatedupto_nonrestarted = winstate->aggregatedupto;
	if (numaggs_restart > 0 &&
		winstate->aggregatedupto != winstate->frameheadpos)
	{
		winstate->aggregatedupto = winstate->frameheadpos;
		ExecClearTuple(agg_row_slot);
	}

	/*
	 * Advance until we reach a row not in frame (or end of partition).
	 *
	 * Note the loop invariant: agg_row_slot is either empty or holds the row
	 * at position aggregatedupto.  We advance aggregatedupto after processing
	 * a row.
	 */
	for (;;)
	{
		/* Fetch next row if we didn't already */
		if (TupIsNull(agg_row_slot))
		{
			if (!window_gettupleslot(agg_winobj, winstate->aggregatedupto,
									 agg_row_slot))
				break;			/* must be end of partition */
		}

		/* Exit loop (for now) if not in frame */
		if (!row_is_in_frame(winstate, winstate->aggregatedupto, agg_row_slot))
			break;

		/* Set tuple context for evaluation of aggregate arguments */
		winstate->tmpcontext->ecxt_outertuple = agg_row_slot;

		/* Accumulate row into the aggregates */
		for (i = 0; i < numaggs; i++)
		{
			peraggstate = &winstate->peragg[i];

			/* Non-restarted aggs skip until aggregatedupto_nonrestarted */
			if (!peraggstate->restart &&
				winstate->aggregatedupto < aggregatedupto_nonrestarted)
				continue;

			wfuncno = peraggstate->wfuncno;
			advance_windowaggregate(winstate,
									&winstate->perfunc[wfuncno],
									peraggstate);
		}

		/* Reset per-input-tuple context after each tuple */
		ResetExprContext(winstate->tmpcontext);

		/* And advance the aggregated-row state */
		winstate->aggregatedupto++;
		ExecClearTuple(agg_row_slot);
	}

	/* The frame's end is not supposed to move backwards, ever */
	Assert(aggregatedupto_nonrestarted <= winstate->aggregatedupto);

	/*
	 * finalize aggregates and fill result/isnull fields.
	 */
	for (i = 0; i < numaggs; i++)
	{
		Datum	   *result;
		bool	   *isnull;

		peraggstate = &winstate->peragg[i];
		wfuncno = peraggstate->wfuncno;
		result = &econtext->ecxt_aggvalues[wfuncno];
		isnull = &econtext->ecxt_aggnulls[wfuncno];
		finalize_windowaggregate(winstate,
								 &winstate->perfunc[wfuncno],
								 peraggstate,
								 result, isnull);

		/*
		 * save the result in case next row shares the same frame.
		 *
		 * XXX in some framing modes, eg ROWS/END_CURRENT_ROW, we can know in
		 * advance that the next row can't possibly share the same frame. Is
		 * it worth detecting that and skipping this code?
		 */
		if (!peraggstate->resulttypeByVal && !*isnull)
		{
			oldContext = MemoryContextSwitchTo(peraggstate->aggcontext);
			peraggstate->resultValue =
				datumCopy(*result,
						  peraggstate->resulttypeByVal,
						  peraggstate->resulttypeLen);
			MemoryContextSwitchTo(oldContext);
		}
		else
		{
			peraggstate->resultValue = *result;
		}
		peraggstate->resultValueIsNull = *isnull;
	}
}

/*
 * eval_windowfunction
 *
 * Arguments of window functions are not evaluated here, because a window
 * function can need random access to arbitrary rows in the partition.
 * The window function uses the special WinGetFuncArgInPartition and
 * WinGetFuncArgInFrame functions to evaluate the arguments for the rows
 * it wants.
 */
static void
eval_windowfunction(WindowAggState *winstate, WindowStatePerFunc perfuncstate,
					Datum *result, bool *isnull)
{
	FunctionCallInfoData fcinfo;
	MemoryContext oldContext;

	oldContext = MemoryContextSwitchTo(winstate->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

	/*
	 * We don't pass any normal arguments to a window function, but we do pass
	 * it the number of arguments, in order to permit window function
	 * implementations to support varying numbers of arguments.  The real info
	 * goes through the WindowObject, which is passed via fcinfo->context.
	 */
	InitFunctionCallInfoData(fcinfo, &(perfuncstate->flinfo),
							 perfuncstate->numArguments,
							 perfuncstate->winCollation,
							 (void *) perfuncstate->winobj, NULL);
	/* Just in case, make all the regular argument slots be null */
	memset(fcinfo.argnull, true, perfuncstate->numArguments);
	/* Window functions don't have a current aggregate context, either */
	winstate->curaggcontext = NULL;

	*result = FunctionCallInvoke(&fcinfo);
	*isnull = fcinfo.isnull;

	/*
	 * Make sure pass-by-ref data is allocated in the appropriate context. (We
	 * need this in case the function returns a pointer into some short-lived
	 * tuple, as is entirely possible.)
	 */
	if (!perfuncstate->resulttypeByVal && !fcinfo.isnull &&
		!MemoryContextContains(CurrentMemoryContext,
							   DatumGetPointer(*result)))
		*result = datumCopy(*result,
							perfuncstate->resulttypeByVal,
							perfuncstate->resulttypeLen);

	MemoryContextSwitchTo(oldContext);
}

/*
 * begin_partition
 * Start buffering rows of the next partition.
 */
static void
begin_partition(WindowAggState *winstate)
{
	PlanState  *outerPlan = outerPlanState(winstate);
	int			numfuncs = winstate->numfuncs;
	int			i;

	winstate->partition_spooled = false;
	winstate->framehead_valid = false;
	winstate->frametail_valid = false;
	winstate->spooled_rows = 0;
	winstate->currentpos = 0;
	winstate->frameheadpos = 0;
	winstate->frametailpos = -1;
	ExecClearTuple(winstate->agg_row_slot);

	/*
	 * If this is the very first partition, we need to fetch the first input
	 * row to store in first_part_slot.
	 */
	if (TupIsNull(winstate->first_part_slot))
	{
		TupleTableSlot *outerslot = ExecProcNode(outerPlan);

		if (!TupIsNull(outerslot))
			ExecCopySlot(winstate->first_part_slot, outerslot);
		else
		{
			/* outer plan is empty, so we have nothing to do */
			winstate->partition_spooled = true;
			winstate->more_partitions = false;
			return;
		}
	}

	/* Create new tuplestore for this partition */
	winstate->buffer = tuplestore_begin_heap(false, false, work_mem);

	/*
	 * Set up read pointers for the tuplestore.  The current pointer doesn't
	 * need BACKWARD capability, but the per-window-function read pointers do,
	 * and the aggregate pointer does if frame start is movable.
	 */
	winstate->current_ptr = 0;	/* read pointer 0 is pre-allocated */

	/* reset default REWIND capability bit for current ptr */
	tuplestore_set_eflags(winstate->buffer, 0);

	/* create read pointers for aggregates, if needed */
	if (winstate->numaggs > 0)
	{
		WindowObject agg_winobj = winstate->agg_winobj;
		int			readptr_flags = 0;

		/* If the frame head is potentially movable ... */
		if (!(winstate->frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING))
		{
			/* ... create a mark pointer to track the frame head */
			agg_winobj->markptr = tuplestore_alloc_read_pointer(winstate->buffer, 0);
			/* and the read pointer will need BACKWARD capability */
			readptr_flags |= EXEC_FLAG_BACKWARD;
		}

		agg_winobj->readptr = tuplestore_alloc_read_pointer(winstate->buffer,
															readptr_flags);
		agg_winobj->markpos = -1;
		agg_winobj->seekpos = -1;

		/* Also reset the row counters for aggregates */
		winstate->aggregatedbase = 0;
		winstate->aggregatedupto = 0;
	}

	/* create mark and read pointers for each real window function */
	for (i = 0; i < numfuncs; i++)
	{
		WindowStatePerFunc perfuncstate = &(winstate->perfunc[i]);

		if (!perfuncstate->plain_agg)
		{
			WindowObject winobj = perfuncstate->winobj;

			winobj->markptr = tuplestore_alloc_read_pointer(winstate->buffer,
															0);
			winobj->readptr = tuplestore_alloc_read_pointer(winstate->buffer,
														 EXEC_FLAG_BACKWARD);
			winobj->markpos = -1;
			winobj->seekpos = -1;
		}
	}

	/*
	 * Store the first tuple into the tuplestore (it's always available now;
	 * we either read it above, or saved it at the end of previous partition)
	 */
	tuplestore_puttupleslot(winstate->buffer, winstate->first_part_slot);
	winstate->spooled_rows++;
}

/*
 * Read tuples from the outer node, up to and including position 'pos', and
 * store them into the tuplestore. If pos is -1, reads the whole partition.
 */
static void
spool_tuples(WindowAggState *winstate, int64 pos)
{
	WindowAgg  *node = (WindowAgg *) winstate->ss.ps.plan;
	PlanState  *outerPlan;
	TupleTableSlot *outerslot;
	MemoryContext oldcontext;

	if (!winstate->buffer)
		return;					/* just a safety check */
	if (winstate->partition_spooled)
		return;					/* whole partition done already */

	/*
	 * If the tuplestore has spilled to disk, alternate reading and writing
	 * becomes quite expensive due to frequent buffer flushes.  It's cheaper
	 * to force the entire partition to get spooled in one go.
	 *
	 * XXX this is a horrid kluge --- it'd be better to fix the performance
	 * problem inside tuplestore.  FIXME
	 */
	if (!tuplestore_in_memory(winstate->buffer))
		pos = -1;

	outerPlan = outerPlanState(winstate);

	/* Must be in query context to call outerplan */
	oldcontext = MemoryContextSwitchTo(winstate->ss.ps.ps_ExprContext->ecxt_per_query_memory);

	while (winstate->spooled_rows <= pos || pos == -1)
	{
		outerslot = ExecProcNode(outerPlan);
		if (TupIsNull(outerslot))
		{
			/* reached the end of the last partition */
			winstate->partition_spooled = true;
			winstate->more_partitions = false;
			break;
		}

		if (node->partNumCols > 0)
		{
			/* Check if this tuple still belongs to the current partition */
			if (!execTuplesMatch(winstate->first_part_slot,
								 outerslot,
								 node->partNumCols, node->partColIdx,
								 winstate->partEqfunctions,
								 winstate->tmpcontext->ecxt_per_tuple_memory))
			{
				/*
				 * end of partition; copy the tuple for the next cycle.
				 */
				ExecCopySlot(winstate->first_part_slot, outerslot);
				winstate->partition_spooled = true;
				winstate->more_partitions = true;
				break;
			}
		}

		/* Still in partition, so save it into the tuplestore */
		tuplestore_puttupleslot(winstate->buffer, outerslot);
		winstate->spooled_rows++;
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 * release_partition
 * clear information kept within a partition, including
 * tuplestore and aggregate results.
 */
static void
release_partition(WindowAggState *winstate)
{
	int			i;

	for (i = 0; i < winstate->numfuncs; i++)
	{
		WindowStatePerFunc perfuncstate = &(winstate->perfunc[i]);

		/* Release any partition-local state of this window function */
		if (perfuncstate->winobj)
			perfuncstate->winobj->localmem = NULL;
	}

	/*
	 * Release all partition-local memory (in particular, any partition-local
	 * state that we might have trashed our pointers to in the above loop, and
	 * any aggregate temp data).  We don't rely on retail pfree because some
	 * aggregates might have allocated data we don't have direct pointers to.
	 */
	MemoryContextResetAndDeleteChildren(winstate->partcontext);
	MemoryContextResetAndDeleteChildren(winstate->aggcontext);
	for (i = 0; i < winstate->numaggs; i++)
	{
		if (winstate->peragg[i].aggcontext != winstate->aggcontext)
			MemoryContextResetAndDeleteChildren(winstate->peragg[i].aggcontext);
	}

	if (winstate->buffer)
		tuplestore_end(winstate->buffer);
	winstate->buffer = NULL;
	winstate->partition_spooled = false;
}

/*
 * row_is_in_frame
 * Determine whether a row is in the current row's window frame according
 * to our window framing rule
 *
 * The caller must have already determined that the row is in the partition
 * and fetched it into a slot.  This function just encapsulates the framing
 * rules.
 */
static bool
row_is_in_frame(WindowAggState *winstate, int64 pos, TupleTableSlot *slot)
{
	int			frameOptions = winstate->frameOptions;

	Assert(pos >= 0);			/* else caller error */

	/* First, check frame starting conditions */
	if (frameOptions & FRAMEOPTION_START_CURRENT_ROW)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* rows before current row are out of frame */
			if (pos < winstate->currentpos)
				return false;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			/* preceding row that is not peer is out of frame */
			if (pos < winstate->currentpos &&
				!are_peers(winstate, slot, winstate->ss.ss_ScanTupleSlot))
				return false;
		}
		else
			Assert(false);
	}
	else if (frameOptions & FRAMEOPTION_START_VALUE)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			int64		offset = DatumGetInt64(winstate->startOffsetValue);

			/* rows before current row + offset are out of frame */
			if (frameOptions & FRAMEOPTION_START_VALUE_PRECEDING)
				offset = -offset;

			if (pos < winstate->currentpos + offset)
				return false;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			/* parser should have rejected this */
			elog(ERROR, "window frame with value offset is not implemented");
		}
		else
			Assert(false);
	}

	/* Okay so far, now check frame ending conditions */
	if (frameOptions & FRAMEOPTION_END_CURRENT_ROW)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* rows after current row are out of frame */
			if (pos > winstate->currentpos)
				return false;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			/* following row that is not peer is out of frame */
			if (pos > winstate->currentpos &&
				!are_peers(winstate, slot, winstate->ss.ss_ScanTupleSlot))
				return false;
		}
		else
			Assert(false);
	}
	else if (frameOptions & FRAMEOPTION_END_VALUE)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			int64		offset = DatumGetInt64(winstate->endOffsetValue);

			/* rows after current row + offset are out of frame */
			if (frameOptions & FRAMEOPTION_END_VALUE_PRECEDING)
				offset = -offset;

			if (pos > winstate->currentpos + offset)
				return false;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			/* parser should have rejected this */
			elog(ERROR, "window frame with value offset is not implemented");
		}
		else
			Assert(false);
	}

	/* If we get here, it's in frame */
	return true;
}

/*
 * update_frameheadpos
 * make frameheadpos valid for the current row
 *
 * Uses the winobj's read pointer for any required fetches; hence, if the
 * frame mode is one that requires row comparisons, the winobj's mark must
 * not be past the currently known frame head.  Also uses the specified slot
 * for any required fetches.
 */
static void
update_frameheadpos(WindowObject winobj, TupleTableSlot *slot)
{
	WindowAggState *winstate = winobj->winstate;
	WindowAgg  *node = (WindowAgg *) winstate->ss.ps.plan;
	int			frameOptions = winstate->frameOptions;

	if (winstate->framehead_valid)
		return;					/* already known for current row */

	if (frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING)
	{
		/* In UNBOUNDED PRECEDING mode, frame head is always row 0 */
		winstate->frameheadpos = 0;
		winstate->framehead_valid = true;
	}
	else if (frameOptions & FRAMEOPTION_START_CURRENT_ROW)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* In ROWS mode, frame head is the same as current */
			winstate->frameheadpos = winstate->currentpos;
			winstate->framehead_valid = true;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			int64		fhprev;

			/* If no ORDER BY, all rows are peers with each other */
			if (node->ordNumCols == 0)
			{
				winstate->frameheadpos = 0;
				winstate->framehead_valid = true;
				return;
			}

			/*
			 * In RANGE START_CURRENT mode, frame head is the first row that
			 * is a peer of current row.  We search backwards from current,
			 * which could be a bit inefficient if peer sets are large. Might
			 * be better to have a separate read pointer that moves forward
			 * tracking the frame head.
			 */
			fhprev = winstate->currentpos - 1;
			for (;;)
			{
				/* assume the frame head can't go backwards */
				if (fhprev < winstate->frameheadpos)
					break;
				if (!window_gettupleslot(winobj, fhprev, slot))
					break;		/* start of partition */
				if (!are_peers(winstate, slot, winstate->ss.ss_ScanTupleSlot))
					break;		/* not peer of current row */
				fhprev--;
			}
			winstate->frameheadpos = fhprev + 1;
			winstate->framehead_valid = true;
		}
		else
			Assert(false);
	}
	else if (frameOptions & FRAMEOPTION_START_VALUE)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* In ROWS mode, bound is physically n before/after current */
			int64		offset = DatumGetInt64(winstate->startOffsetValue);

			if (frameOptions & FRAMEOPTION_START_VALUE_PRECEDING)
				offset = -offset;

			winstate->frameheadpos = winstate->currentpos + offset;
			/* frame head can't go before first row */
			if (winstate->frameheadpos < 0)
				winstate->frameheadpos = 0;
			else if (winstate->frameheadpos > winstate->currentpos)
			{
				/* make sure frameheadpos is not past end of partition */
				spool_tuples(winstate, winstate->frameheadpos - 1);
				if (winstate->frameheadpos > winstate->spooled_rows)
					winstate->frameheadpos = winstate->spooled_rows;
			}
			winstate->framehead_valid = true;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			/* parser should have rejected this */
			elog(ERROR, "window frame with value offset is not implemented");
		}
		else
			Assert(false);
	}
	else
		Assert(false);
}

/*
 * update_frametailpos
 * make frametailpos valid for the current row
 *
 * Uses the winobj's read pointer for any required fetches; hence, if the
 * frame mode is one that requires row comparisons, the winobj's mark must
 * not be past the currently known frame tail.  Also uses the specified slot
 * for any required fetches.
 */
static void
update_frametailpos(WindowObject winobj, TupleTableSlot *slot)
{
	WindowAggState *winstate = winobj->winstate;
	WindowAgg  *node = (WindowAgg *) winstate->ss.ps.plan;
	int			frameOptions = winstate->frameOptions;

	if (winstate->frametail_valid)
		return;					/* already known for current row */

	if (frameOptions & FRAMEOPTION_END_UNBOUNDED_FOLLOWING)
	{
		/* In UNBOUNDED FOLLOWING mode, all partition rows are in frame */
		spool_tuples(winstate, -1);
		winstate->frametailpos = winstate->spooled_rows - 1;
		winstate->frametail_valid = true;
	}
	else if (frameOptions & FRAMEOPTION_END_CURRENT_ROW)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* In ROWS mode, exactly the rows up to current are in frame */
			winstate->frametailpos = winstate->currentpos;
			winstate->frametail_valid = true;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			int64		ftnext;

			/* If no ORDER BY, all rows are peers with each other */
			if (node->ordNumCols == 0)
			{
				spool_tuples(winstate, -1);
				winstate->frametailpos = winstate->spooled_rows - 1;
				winstate->frametail_valid = true;
				return;
			}

			/*
			 * Else we have to search for the first non-peer of the current
			 * row.  We assume the current value of frametailpos is a lower
			 * bound on the possible frame tail location, ie, frame tail never
			 * goes backward, and that currentpos is also a lower bound, ie,
			 * frame end always >= current row.
			 */
			ftnext = Max(winstate->frametailpos, winstate->currentpos) + 1;
			for (;;)
			{
				if (!window_gettupleslot(winobj, ftnext, slot))
					break;		/* end of partition */
				if (!are_peers(winstate, slot, winstate->ss.ss_ScanTupleSlot))
					break;		/* not peer of current row */
				ftnext++;
			}
			winstate->frametailpos = ftnext - 1;
			winstate->frametail_valid = true;
		}
		else
			Assert(false);
	}
	else if (frameOptions & FRAMEOPTION_END_VALUE)
	{
		if (frameOptions & FRAMEOPTION_ROWS)
		{
			/* In ROWS mode, bound is physically n before/after current */
			int64		offset = DatumGetInt64(winstate->endOffsetValue);

			if (frameOptions & FRAMEOPTION_END_VALUE_PRECEDING)
				offset = -offset;

			winstate->frametailpos = winstate->currentpos + offset;
			/* smallest allowable value of frametailpos is -1 */
			if (winstate->frametailpos < 0)
				winstate->frametailpos = -1;
			else if (winstate->frametailpos > winstate->currentpos)
			{
				/* make sure frametailpos is not past last row of partition */
				spool_tuples(winstate, winstate->frametailpos);
				if (winstate->frametailpos >= winstate->spooled_rows)
					winstate->frametailpos = winstate->spooled_rows - 1;
			}
			winstate->frametail_valid = true;
		}
		else if (frameOptions & FRAMEOPTION_RANGE)
		{
			/* parser should have rejected this */
			elog(ERROR, "window frame with value offset is not implemented");
		}
		else
			Assert(false);
	}
	else
		Assert(false);
}


/* -----------------
 * ExecWindowAgg
 *
 *	ExecWindowAgg receives tuples from its outer subplan and
 *	stores them into a tuplestore, then processes window functions.
 *	This node doesn't reduce nor qualify any row so the number of
 *	returned rows is exactly the same as its outer subplan's result
 *	(ignoring the case of SRFs in the targetlist, that is).
 * -----------------
 */
TupleTableSlot *
ExecWindowAgg(WindowAggState *winstate)
{
	TupleTableSlot *result;
	ExprDoneCond isDone;
	ExprContext *econtext;
	int			i;
	int			numfuncs;

	if (winstate->all_done)
		return NULL;

	/*
	 * Check to see if we're still projecting out tuples from a previous
	 * output tuple (because there is a function-returning-set in the
	 * projection expressions).  If so, try to project another one.
	 */
	if (winstate->ss.ps.ps_TupFromTlist)
	{
		TupleTableSlot *result;
		ExprDoneCond isDone;

		result = ExecProject(winstate->ss.ps.ps_ProjInfo, &isDone);
		if (isDone == ExprMultipleResult)
			return result;
		/* Done with that source tuple... */
		winstate->ss.ps.ps_TupFromTlist = false;
	}

	/*
	 * Compute frame offset values, if any, during first call.
	 */
	if (winstate->all_first)
	{
		int			frameOptions = winstate->frameOptions;
		ExprContext *econtext = winstate->ss.ps.ps_ExprContext;
		Datum		value;
		bool		isnull;
		int16		len;
		bool		byval;

		if (frameOptions & FRAMEOPTION_START_VALUE)
		{
			Assert(winstate->startOffset != NULL);
			value = ExecEvalExprSwitchContext(winstate->startOffset,
											  econtext,
											  &isnull,
											  NULL);
			if (isnull)
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						 errmsg("frame starting offset must not be null")));
			/* copy value into query-lifespan context */
			get_typlenbyval(exprType((Node *) winstate->startOffset->expr),
							&len, &byval);
			winstate->startOffsetValue = datumCopy(value, byval, len);
			if (frameOptions & FRAMEOPTION_ROWS)
			{
				/* value is known to be int8 */
				int64		offset = DatumGetInt64(value);

				if (offset < 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					  errmsg("frame starting offset must not be negative")));
			}
		}
		if (frameOptions & FRAMEOPTION_END_VALUE)
		{
			Assert(winstate->endOffset != NULL);
			value = ExecEvalExprSwitchContext(winstate->endOffset,
											  econtext,
											  &isnull,
											  NULL);
			if (isnull)
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						 errmsg("frame ending offset must not be null")));
			/* copy value into query-lifespan context */
			get_typlenbyval(exprType((Node *) winstate->endOffset->expr),
							&len, &byval);
			winstate->endOffsetValue = datumCopy(value, byval, len);
			if (frameOptions & FRAMEOPTION_ROWS)
			{
				/* value is known to be int8 */
				int64		offset = DatumGetInt64(value);

				if (offset < 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("frame ending offset must not be negative")));
			}
		}
		winstate->all_first = false;
	}

restart:
	if (winstate->buffer == NULL)
	{
		/* Initialize for first partition and set current row = 0 */
		begin_partition(winstate);
		/* If there are no input rows, we'll detect that and exit below */
	}
	else
	{
		/* Advance current row within partition */
		winstate->currentpos++;
		/* This might mean that the frame moves, too */
		winstate->framehead_valid = false;
		winstate->frametail_valid = false;
	}

	/*
	 * Spool all tuples up to and including the current row, if we haven't
	 * already
	 */
	spool_tuples(winstate, winstate->currentpos);

	/* Move to the next partition if we reached the end of this partition */
	if (winstate->partition_spooled &&
		winstate->currentpos >= winstate->spooled_rows)
	{
		release_partition(winstate);

		if (winstate->more_partitions)
		{
			begin_partition(winstate);
			Assert(winstate->spooled_rows > 0);
		}
		else
		{
			winstate->all_done = true;
			return NULL;
		}
	}

	/* final output execution is in ps_ExprContext */
	econtext = winstate->ss.ps.ps_ExprContext;

	/* Clear the per-output-tuple context for current row */
	ResetExprContext(econtext);

	/*
	 * Read the current row from the tuplestore, and save in ScanTupleSlot.
	 * (We can't rely on the outerplan's output slot because we may have to
	 * read beyond the current row.  Also, we have to actually copy the row
	 * out of the tuplestore, since window function evaluation might cause the
	 * tuplestore to dump its state to disk.)
	 *
	 * Current row must be in the tuplestore, since we spooled it above.
	 */
	tuplestore_select_read_pointer(winstate->buffer, winstate->current_ptr);
	if (!tuplestore_gettupleslot(winstate->buffer, true, true,
								 winstate->ss.ss_ScanTupleSlot))
		elog(ERROR, "unexpected end of tuplestore");

	/*
	 * Evaluate true window functions
	 */
	numfuncs = winstate->numfuncs;
	for (i = 0; i < numfuncs; i++)
	{
		WindowStatePerFunc perfuncstate = &(winstate->perfunc[i]);

		if (perfuncstate->plain_agg)
			continue;
		eval_windowfunction(winstate, perfuncstate,
			  &(econtext->ecxt_aggvalues[perfuncstate->wfuncstate->wfuncno]),
			  &(econtext->ecxt_aggnulls[perfuncstate->wfuncstate->wfuncno]));
	}

	/*
	 * Evaluate aggregates
	 */
	if (winstate->numaggs > 0)
		eval_windowaggregates(winstate);

	/*
	 * Truncate any no-longer-needed rows from the tuplestore.
	 */
	tuplestore_trim(winstate->buffer);

	/*
	 * Form and return a projection tuple using the windowfunc results and the
	 * current row.  Setting ecxt_outertuple arranges that any Vars will be
	 * evaluated with respect to that row.
	 */
	econtext->ecxt_outertuple = winstate->ss.ss_ScanTupleSlot;
	result = ExecProject(winstate->ss.ps.ps_ProjInfo, &isDone);

	if (isDone == ExprEndResult)
	{
		/* SRF in tlist returned no rows, so advance to next input tuple */
		goto restart;
	}

	winstate->ss.ps.ps_TupFromTlist =
		(isDone == ExprMultipleResult);
	return result;
}

/* -----------------
 * ExecInitWindowAgg
 *
 *	Creates the run-time information for the WindowAgg node produced by the
 *	planner and initializes its outer subtree
 * -----------------
 */
WindowAggState *
ExecInitWindowAgg(WindowAgg *node, EState *estate, int eflags)
{
	WindowAggState *winstate;
	Plan	   *outerPlan;
	ExprContext *econtext;
	ExprContext *tmpcontext;
	WindowStatePerFunc perfunc;
	WindowStatePerAgg peragg;
	int			numfuncs,
				wfuncno,
				numaggs,
				aggno;
	ListCell   *l;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	winstate = makeNode(WindowAggState);
	winstate->ss.ps.plan = (Plan *) node;
	winstate->ss.ps.state = estate;

	/*
	 * Create expression contexts.  We need two, one for per-input-tuple
	 * processing and one for per-output-tuple processing.  We cheat a little
	 * by using ExecAssignExprContext() to build both.
	 */
	ExecAssignExprContext(estate, &winstate->ss.ps);
	tmpcontext = winstate->ss.ps.ps_ExprContext;
	winstate->tmpcontext = tmpcontext;
	ExecAssignExprContext(estate, &winstate->ss.ps);

	/* Create long-lived context for storage of partition-local memory etc */
	winstate->partcontext =
		AllocSetContextCreate(CurrentMemoryContext,
							  "WindowAgg_Partition",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * Create mid-lived context for aggregate trans values etc.
	 *
	 * Note that moving aggregates each use their own private context, not
	 * this one.
	 */
	winstate->aggcontext =
		AllocSetContextCreate(CurrentMemoryContext,
							  "WindowAgg_Aggregates",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * tuple table initialization
	 */
	ExecInitScanTupleSlot(estate, &winstate->ss);
	ExecInitResultTupleSlot(estate, &winstate->ss.ps);
	winstate->first_part_slot = ExecInitExtraTupleSlot(estate);
	winstate->agg_row_slot = ExecInitExtraTupleSlot(estate);
	winstate->temp_slot_1 = ExecInitExtraTupleSlot(estate);
	winstate->temp_slot_2 = ExecInitExtraTupleSlot(estate);

	winstate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->plan.targetlist,
					 (PlanState *) winstate);

	/*
	 * WindowAgg nodes never have quals, since they can only occur at the
	 * logical top level of a query (ie, after any WHERE or HAVING filters)
	 */
	Assert(node->plan.qual == NIL);
	winstate->ss.ps.qual = NIL;

	/*
	 * initialize child nodes
	 */
	outerPlan = outerPlan(node);
	outerPlanState(winstate) = ExecInitNode(outerPlan, estate, eflags);

	/*
	 * initialize source tuple type (which is also the tuple type that we'll
	 * store in the tuplestore and use in all our working slots).
	 */
	ExecAssignScanTypeFromOuterPlan(&winstate->ss);

	ExecSetSlotDescriptor(winstate->first_part_slot,
						  winstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor);
	ExecSetSlotDescriptor(winstate->agg_row_slot,
						  winstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor);
	ExecSetSlotDescriptor(winstate->temp_slot_1,
						  winstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor);
	ExecSetSlotDescriptor(winstate->temp_slot_2,
						  winstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor);

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&winstate->ss.ps);
	ExecAssignProjectionInfo(&winstate->ss.ps, NULL);

	winstate->ss.ps.ps_TupFromTlist = false;

	/* Set up data for comparing tuples */
	if (node->partNumCols > 0)
		winstate->partEqfunctions = execTuplesMatchPrepare(node->partNumCols,
														node->partOperators);
	if (node->ordNumCols > 0)
		winstate->ordEqfunctions = execTuplesMatchPrepare(node->ordNumCols,
														  node->ordOperators);

	/*
	 * WindowAgg nodes use aggvalues and aggnulls as well as Agg nodes.
	 */
	numfuncs = winstate->numfuncs;
	numaggs = winstate->numaggs;
	econtext = winstate->ss.ps.ps_ExprContext;
	econtext->ecxt_aggvalues = (Datum *) palloc0(sizeof(Datum) * numfuncs);
	econtext->ecxt_aggnulls = (bool *) palloc0(sizeof(bool) * numfuncs);

	/*
	 * allocate per-wfunc/per-agg state information.
	 */
	perfunc = (WindowStatePerFunc) palloc0(sizeof(WindowStatePerFuncData) * numfuncs);
	peragg = (WindowStatePerAgg) palloc0(sizeof(WindowStatePerAggData) * numaggs);
	winstate->perfunc = perfunc;
	winstate->peragg = peragg;

	wfuncno = -1;
	aggno = -1;
	foreach(l, winstate->funcs)
	{
		WindowFuncExprState *wfuncstate = (WindowFuncExprState *) lfirst(l);
		WindowFunc *wfunc = (WindowFunc *) wfuncstate->xprstate.expr;
		WindowStatePerFunc perfuncstate;
		AclResult	aclresult;
		int			i;

		if (wfunc->winref != node->winref)		/* planner screwed up? */
			elog(ERROR, "WindowFunc with winref %u assigned to WindowAgg with winref %u",
				 wfunc->winref, node->winref);

		/* Look for a previous duplicate window function */
		for (i = 0; i <= wfuncno; i++)
		{
			if (equal(wfunc, perfunc[i].wfunc) &&
				!contain_volatile_functions((Node *) wfunc))
				break;
		}
		if (i <= wfuncno)
		{
			/* Found a match to an existing entry, so just mark it */
			wfuncstate->wfuncno = i;
			continue;
		}

		/* Nope, so assign a new PerAgg record */
		perfuncstate = &perfunc[++wfuncno];

		/* Mark WindowFunc state node with assigned index in the result array */
		wfuncstate->wfuncno = wfuncno;

		/* Check permission to call window function */
		aclresult = pg_proc_aclcheck(wfunc->winfnoid, GetUserId(),
									 ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_PROC,
						   get_func_name(wfunc->winfnoid));
		InvokeFunctionExecuteHook(wfunc->winfnoid);

		/* Fill in the perfuncstate data */
		perfuncstate->wfuncstate = wfuncstate;
		perfuncstate->wfunc = wfunc;
		perfuncstate->numArguments = list_length(wfuncstate->args);

		fmgr_info_cxt(wfunc->winfnoid, &perfuncstate->flinfo,
					  econtext->ecxt_per_query_memory);
		fmgr_info_set_expr((Node *) wfunc, &perfuncstate->flinfo);

		perfuncstate->winCollation = wfunc->inputcollid;

		get_typlenbyval(wfunc->wintype,
						&perfuncstate->resulttypeLen,
						&perfuncstate->resulttypeByVal);

		/*
		 * If it's really just a plain aggregate function, we'll emulate the
		 * Agg environment for it.
		 */
		perfuncstate->plain_agg = wfunc->winagg;
		if (wfunc->winagg)
		{
			WindowStatePerAgg peraggstate;

			perfuncstate->aggno = ++aggno;
			peraggstate = &winstate->peragg[aggno];
			initialize_peragg(winstate, wfunc, peraggstate);
			peraggstate->wfuncno = wfuncno;
		}
		else
		{
			WindowObject winobj = makeNode(WindowObjectData);

			winobj->winstate = winstate;
			winobj->argstates = wfuncstate->args;
			winobj->localmem = NULL;
			perfuncstate->winobj = winobj;
		}
	}

	/* Update numfuncs, numaggs to match number of unique functions found */
	winstate->numfuncs = wfuncno + 1;
	winstate->numaggs = aggno + 1;

	/* Set up WindowObject for aggregates, if needed */
	if (winstate->numaggs > 0)
	{
		WindowObject agg_winobj = makeNode(WindowObjectData);

		agg_winobj->winstate = winstate;
		agg_winobj->argstates = NIL;
		agg_winobj->localmem = NULL;
		/* make sure markptr = -1 to invalidate. It may not get used */
		agg_winobj->markptr = -1;
		agg_winobj->readptr = -1;
		winstate->agg_winobj = agg_winobj;
	}

	/* copy frame options to state node for easy access */
	winstate->frameOptions = node->frameOptions;

	/* initialize frame bound offset expressions */
	winstate->startOffset = ExecInitExpr((Expr *) node->startOffset,
										 (PlanState *) winstate);
	winstate->endOffset = ExecInitExpr((Expr *) node->endOffset,
									   (PlanState *) winstate);

	winstate->all_first = true;
	winstate->partition_spooled = false;
	winstate->more_partitions = false;

	return winstate;
}

/* -----------------
 * ExecEndWindowAgg
 * -----------------
 */
void
ExecEndWindowAgg(WindowAggState *node)
{
	PlanState  *outerPlan;
	int			i;

	release_partition(node);

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	ExecClearTuple(node->first_part_slot);
	ExecClearTuple(node->agg_row_slot);
	ExecClearTuple(node->temp_slot_1);
	ExecClearTuple(node->temp_slot_2);

	/*
	 * Free both the expr contexts.
	 */
	ExecFreeExprContext(&node->ss.ps);
	node->ss.ps.ps_ExprContext = node->tmpcontext;
	ExecFreeExprContext(&node->ss.ps);

	for (i = 0; i < node->numaggs; i++)
	{
		if (node->peragg[i].aggcontext != node->aggcontext)
			MemoryContextDelete(node->peragg[i].aggcontext);
	}
	MemoryContextDelete(node->partcontext);
	MemoryContextDelete(node->aggcontext);

	pfree(node->perfunc);
	pfree(node->peragg);

	outerPlan = outerPlanState(node);
	ExecEndNode(outerPlan);
}

/* -----------------
 * ExecReScanWindowAgg
 * -----------------
 */
void
ExecReScanWindowAgg(WindowAggState *node)
{
	PlanState  *outerPlan = outerPlanState(node);
	ExprContext *econtext = node->ss.ps.ps_ExprContext;

	node->all_done = false;

	node->ss.ps.ps_TupFromTlist = false;
	node->all_first = true;

	/* release tuplestore et al */
	release_partition(node);

	/* release all temp tuples, but especially first_part_slot */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	ExecClearTuple(node->first_part_slot);
	ExecClearTuple(node->agg_row_slot);
	ExecClearTuple(node->temp_slot_1);
	ExecClearTuple(node->temp_slot_2);

	/* Forget current wfunc values */
	MemSet(econtext->ecxt_aggvalues, 0, sizeof(Datum) * node->numfuncs);
	MemSet(econtext->ecxt_aggnulls, 0, sizeof(bool) * node->numfuncs);

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}

/*
 * initialize_peragg
 *
 * Almost same as in nodeAgg.c, except we don't support DISTINCT currently.
 */
static WindowStatePerAggData *
initialize_peragg(WindowAggState *winstate, WindowFunc *wfunc,
				  WindowStatePerAgg peraggstate)
{
	Oid			inputTypes[FUNC_MAX_ARGS];
	int			numArguments;
	HeapTuple	aggTuple;
	Form_pg_aggregate aggform;
	Oid			aggtranstype;
	AttrNumber	initvalAttNo;
	AclResult	aclresult;
	Oid			transfn_oid,
				invtransfn_oid,
				finalfn_oid;
	bool		finalextra;
	Expr	   *transfnexpr,
			   *invtransfnexpr,
			   *finalfnexpr;
	Datum		textInitVal;
	int			i;
	ListCell   *lc;

	numArguments = list_length(wfunc->args);

	i = 0;
	foreach(lc, wfunc->args)
	{
		inputTypes[i++] = exprType((Node *) lfirst(lc));
	}

	aggTuple = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(wfunc->winfnoid));
	if (!HeapTupleIsValid(aggTuple))
		elog(ERROR, "cache lookup failed for aggregate %u",
			 wfunc->winfnoid);
	aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);

	/*
	 * Figure out whether we want to use the moving-aggregate implementation,
	 * and collect the right set of fields from the pg_attribute entry.
	 *
	 * If the frame head can't move, we don't need moving-aggregate code. Even
	 * if we'd like to use it, don't do so if the aggregate's arguments (and
	 * FILTER clause if any) contain any calls to volatile functions.
	 * Otherwise, the difference between restarting and not restarting the
	 * aggregation would be user-visible.
	 */
	if (OidIsValid(aggform->aggminvtransfn) &&
		!(winstate->frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING) &&
		!contain_volatile_functions((Node *) wfunc))
	{
		peraggstate->transfn_oid = transfn_oid = aggform->aggmtransfn;
		peraggstate->invtransfn_oid = invtransfn_oid = aggform->aggminvtransfn;
		peraggstate->finalfn_oid = finalfn_oid = aggform->aggmfinalfn;
		finalextra = aggform->aggmfinalextra;
		aggtranstype = aggform->aggmtranstype;
		initvalAttNo = Anum_pg_aggregate_aggminitval;
	}
	else
	{
		peraggstate->transfn_oid = transfn_oid = aggform->aggtransfn;
		peraggstate->invtransfn_oid = invtransfn_oid = InvalidOid;
		peraggstate->finalfn_oid = finalfn_oid = aggform->aggfinalfn;
		finalextra = aggform->aggfinalextra;
		aggtranstype = aggform->aggtranstype;
		initvalAttNo = Anum_pg_aggregate_agginitval;
	}

	/*
	 * ExecInitWindowAgg already checked permission to call aggregate function
	 * ... but we still need to check the component functions
	 */

	/* Check that aggregate owner has permission to call component fns */
	{
		HeapTuple	procTuple;
		Oid			aggOwner;

		procTuple = SearchSysCache1(PROCOID,
									ObjectIdGetDatum(wfunc->winfnoid));
		if (!HeapTupleIsValid(procTuple))
			elog(ERROR, "cache lookup failed for function %u",
				 wfunc->winfnoid);
		aggOwner = ((Form_pg_proc) GETSTRUCT(procTuple))->proowner;
		ReleaseSysCache(procTuple);

		aclresult = pg_proc_aclcheck(transfn_oid, aggOwner,
									 ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_PROC,
						   get_func_name(transfn_oid));
		InvokeFunctionExecuteHook(transfn_oid);

		if (OidIsValid(invtransfn_oid))
		{
			aclresult = pg_proc_aclcheck(invtransfn_oid, aggOwner,
										 ACL_EXECUTE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, ACL_KIND_PROC,
							   get_func_name(invtransfn_oid));
			InvokeFunctionExecuteHook(invtransfn_oid);
		}

		if (OidIsValid(finalfn_oid))
		{
			aclresult = pg_proc_aclcheck(finalfn_oid, aggOwner,
										 ACL_EXECUTE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, ACL_KIND_PROC,
							   get_func_name(finalfn_oid));
			InvokeFunctionExecuteHook(finalfn_oid);
		}
	}

	/* Detect how many arguments to pass to the finalfn */
	if (finalextra)
		peraggstate->numFinalArgs = numArguments + 1;
	else
		peraggstate->numFinalArgs = 1;

	/* resolve actual type of transition state, if polymorphic */
	aggtranstype = resolve_aggregate_transtype(wfunc->winfnoid,
											   aggtranstype,
											   inputTypes,
											   numArguments);

	/* build expression trees using actual argument & result types */
	build_aggregate_fnexprs(inputTypes,
							numArguments,
							0,	/* no ordered-set window functions yet */
							peraggstate->numFinalArgs,
							false,		/* no variadic window functions yet */
							aggtranstype,
							wfunc->wintype,
							wfunc->inputcollid,
							transfn_oid,
							invtransfn_oid,
							finalfn_oid,
							&transfnexpr,
							&invtransfnexpr,
							&finalfnexpr);

	/* set up infrastructure for calling the transfn(s) and finalfn */
	fmgr_info(transfn_oid, &peraggstate->transfn);
	fmgr_info_set_expr((Node *) transfnexpr, &peraggstate->transfn);

	if (OidIsValid(invtransfn_oid))
	{
		fmgr_info(invtransfn_oid, &peraggstate->invtransfn);
		fmgr_info_set_expr((Node *) invtransfnexpr, &peraggstate->invtransfn);
	}

	if (OidIsValid(finalfn_oid))
	{
		fmgr_info(finalfn_oid, &peraggstate->finalfn);
		fmgr_info_set_expr((Node *) finalfnexpr, &peraggstate->finalfn);
	}

	/* get info about relevant datatypes */
	get_typlenbyval(wfunc->wintype,
					&peraggstate->resulttypeLen,
					&peraggstate->resulttypeByVal);
	get_typlenbyval(aggtranstype,
					&peraggstate->transtypeLen,
					&peraggstate->transtypeByVal);

	/*
	 * initval is potentially null, so don't try to access it as a struct
	 * field. Must do it the hard way with SysCacheGetAttr.
	 */
	textInitVal = SysCacheGetAttr(AGGFNOID, aggTuple, initvalAttNo,
								  &peraggstate->initValueIsNull);

	if (peraggstate->initValueIsNull)
		peraggstate->initValue = (Datum) 0;
	else
		peraggstate->initValue = GetAggInitVal(textInitVal,
											   aggtranstype);

	/*
	 * If the transfn is strict and the initval is NULL, make sure input type
	 * and transtype are the same (or at least binary-compatible), so that
	 * it's OK to use the first input value as the initial transValue.  This
	 * should have been checked at agg definition time, but we must check
	 * again in case the transfn's strictness property has been changed.
	 */
	if (peraggstate->transfn.fn_strict && peraggstate->initValueIsNull)
	{
		if (numArguments < 1 ||
			!IsBinaryCoercible(inputTypes[0], aggtranstype))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate %u needs to have compatible input type and transition type",
							wfunc->winfnoid)));
	}

	/*
	 * Insist that forward and inverse transition functions have the same
	 * strictness setting.  Allowing them to differ would require handling
	 * more special cases in advance_windowaggregate and
	 * advance_windowaggregate_base, for no discernible benefit.  This should
	 * have been checked at agg definition time, but we must check again in
	 * case either function's strictness property has been changed.
	 */
	if (OidIsValid(invtransfn_oid) &&
		peraggstate->transfn.fn_strict != peraggstate->invtransfn.fn_strict)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("strictness of aggregate's forward and inverse transition functions must match")));

	/*
	 * Moving aggregates use their own aggcontext.
	 *
	 * This is necessary because they might restart at different times, so we
	 * might never be able to reset the shared context otherwise.  We can't
	 * make it the aggregates' responsibility to clean up after themselves,
	 * because strict aggregates must be restarted whenever we remove their
	 * last non-NULL input, which the aggregate won't be aware is happening.
	 * Also, just pfree()ing the transValue upon restarting wouldn't help,
	 * since we'd miss any indirectly referenced data.  We could, in theory,
	 * make the memory allocation rules for moving aggregates different than
	 * they have historically been for plain aggregates, but that seems grotty
	 * and likely to lead to memory leaks.
	 */
	if (OidIsValid(invtransfn_oid))
		peraggstate->aggcontext =
			AllocSetContextCreate(CurrentMemoryContext,
								  "WindowAgg_AggregatePrivate",
								  ALLOCSET_DEFAULT_MINSIZE,
								  ALLOCSET_DEFAULT_INITSIZE,
								  ALLOCSET_DEFAULT_MAXSIZE);
	else
		peraggstate->aggcontext = winstate->aggcontext;

	ReleaseSysCache(aggTuple);

	return peraggstate;
}

static Datum
GetAggInitVal(Datum textInitVal, Oid transtype)
{
	Oid			typinput,
				typioparam;
	char	   *strInitVal;
	Datum		initVal;

	getTypeInputInfo(transtype, &typinput, &typioparam);
	strInitVal = TextDatumGetCString(textInitVal);
	initVal = OidInputFunctionCall(typinput, strInitVal,
								   typioparam, -1);
	pfree(strInitVal);
	return initVal;
}

/*
 * are_peers
 * compare two rows to see if they are equal according to the ORDER BY clause
 *
 * NB: this does not consider the window frame mode.
 */
static bool
are_peers(WindowAggState *winstate, TupleTableSlot *slot1,
		  TupleTableSlot *slot2)
{
	WindowAgg  *node = (WindowAgg *) winstate->ss.ps.plan;

	/* If no ORDER BY, all rows are peers with each other */
	if (node->ordNumCols == 0)
		return true;

	return execTuplesMatch(slot1, slot2,
						   node->ordNumCols, node->ordColIdx,
						   winstate->ordEqfunctions,
						   winstate->tmpcontext->ecxt_per_tuple_memory);
}

/*
 * window_gettupleslot
 *	Fetch the pos'th tuple of the current partition into the slot,
 *	using the winobj's read pointer
 *
 * Returns true if successful, false if no such row
 */
static bool
window_gettupleslot(WindowObject winobj, int64 pos, TupleTableSlot *slot)
{
	WindowAggState *winstate = winobj->winstate;
	MemoryContext oldcontext;

	/* Don't allow passing -1 to spool_tuples here */
	if (pos < 0)
		return false;

	/* If necessary, fetch the tuple into the spool */
	spool_tuples(winstate, pos);

	if (pos >= winstate->spooled_rows)
		return false;

	if (pos < winobj->markpos)
		elog(ERROR, "cannot fetch row before WindowObject's mark position");

	oldcontext = MemoryContextSwitchTo(winstate->ss.ps.ps_ExprContext->ecxt_per_query_memory);

	tuplestore_select_read_pointer(winstate->buffer, winobj->readptr);

	/*
	 * Advance or rewind until we are within one tuple of the one we want.
	 */
	if (winobj->seekpos < pos - 1)
	{
		if (!tuplestore_skiptuples(winstate->buffer,
								   pos - 1 - winobj->seekpos,
								   true))
			elog(ERROR, "unexpected end of tuplestore");
		winobj->seekpos = pos - 1;
	}
	else if (winobj->seekpos > pos + 1)
	{
		if (!tuplestore_skiptuples(winstate->buffer,
								   winobj->seekpos - (pos + 1),
								   false))
			elog(ERROR, "unexpected end of tuplestore");
		winobj->seekpos = pos + 1;
	}
	else if (winobj->seekpos == pos)
	{
		/*
		 * There's no API to refetch the tuple at the current position.  We
		 * have to move one tuple forward, and then one backward.  (We don't
		 * do it the other way because we might try to fetch the row before
		 * our mark, which isn't allowed.)  XXX this case could stand to be
		 * optimized.
		 */
		tuplestore_advance(winstate->buffer, true);
		winobj->seekpos++;
	}

	/*
	 * Now we should be on the tuple immediately before or after the one we
	 * want, so just fetch forwards or backwards as appropriate.
	 */
	if (winobj->seekpos > pos)
	{
		if (!tuplestore_gettupleslot(winstate->buffer, false, true, slot))
			elog(ERROR, "unexpected end of tuplestore");
		winobj->seekpos--;
	}
	else
	{
		if (!tuplestore_gettupleslot(winstate->buffer, true, true, slot))
			elog(ERROR, "unexpected end of tuplestore");
		winobj->seekpos++;
	}

	Assert(winobj->seekpos == pos);

	MemoryContextSwitchTo(oldcontext);

	return true;
}


/***********************************************************************
 * API exposed to window functions
 ***********************************************************************/


/*
 * WinGetPartitionLocalMemory
 *		Get working memory that lives till end of partition processing
 *
 * On first call within a given partition, this allocates and zeroes the
 * requested amount of space.  Subsequent calls just return the same chunk.
 *
 * Memory obtained this way is normally used to hold state that should be
 * automatically reset for each new partition.  If a window function wants
 * to hold state across the whole query, fcinfo->fn_extra can be used in the
 * usual way for that.
 */
void *
WinGetPartitionLocalMemory(WindowObject winobj, Size sz)
{
	Assert(WindowObjectIsValid(winobj));
	if (winobj->localmem == NULL)
		winobj->localmem =
			MemoryContextAllocZero(winobj->winstate->partcontext, sz);
	return winobj->localmem;
}

/*
 * WinGetCurrentPosition
 *		Return the current row's position (counting from 0) within the current
 *		partition.
 */
int64
WinGetCurrentPosition(WindowObject winobj)
{
	Assert(WindowObjectIsValid(winobj));
	return winobj->winstate->currentpos;
}

/*
 * WinGetPartitionRowCount
 *		Return total number of rows contained in the current partition.
 *
 * Note: this is a relatively expensive operation because it forces the
 * whole partition to be "spooled" into the tuplestore at once.  Once
 * executed, however, additional calls within the same partition are cheap.
 */
int64
WinGetPartitionRowCount(WindowObject winobj)
{
	Assert(WindowObjectIsValid(winobj));
	spool_tuples(winobj->winstate, -1);
	return winobj->winstate->spooled_rows;
}

/*
 * WinSetMarkPosition
 *		Set the "mark" position for the window object, which is the oldest row
 *		number (counting from 0) it is allowed to fetch during all subsequent
 *		operations within the current partition.
 *
 * Window functions do not have to call this, but are encouraged to move the
 * mark forward when possible to keep the tuplestore size down and prevent
 * having to spill rows to disk.
 */
void
WinSetMarkPosition(WindowObject winobj, int64 markpos)
{
	WindowAggState *winstate;

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;

	if (markpos < winobj->markpos)
		elog(ERROR, "cannot move WindowObject's mark position backward");
	tuplestore_select_read_pointer(winstate->buffer, winobj->markptr);
	if (markpos > winobj->markpos)
	{
		tuplestore_skiptuples(winstate->buffer,
							  markpos - winobj->markpos,
							  true);
		winobj->markpos = markpos;
	}
	tuplestore_select_read_pointer(winstate->buffer, winobj->readptr);
	if (markpos > winobj->seekpos)
	{
		tuplestore_skiptuples(winstate->buffer,
							  markpos - winobj->seekpos,
							  true);
		winobj->seekpos = markpos;
	}
}

/*
 * WinRowsArePeers
 *		Compare two rows (specified by absolute position in window) to see
 *		if they are equal according to the ORDER BY clause.
 *
 * NB: this does not consider the window frame mode.
 */
bool
WinRowsArePeers(WindowObject winobj, int64 pos1, int64 pos2)
{
	WindowAggState *winstate;
	WindowAgg  *node;
	TupleTableSlot *slot1;
	TupleTableSlot *slot2;
	bool		res;

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;
	node = (WindowAgg *) winstate->ss.ps.plan;

	/* If no ORDER BY, all rows are peers; don't bother to fetch them */
	if (node->ordNumCols == 0)
		return true;

	slot1 = winstate->temp_slot_1;
	slot2 = winstate->temp_slot_2;

	if (!window_gettupleslot(winobj, pos1, slot1))
		elog(ERROR, "specified position is out of window: " INT64_FORMAT,
			 pos1);
	if (!window_gettupleslot(winobj, pos2, slot2))
		elog(ERROR, "specified position is out of window: " INT64_FORMAT,
			 pos2);

	res = are_peers(winstate, slot1, slot2);

	ExecClearTuple(slot1);
	ExecClearTuple(slot2);

	return res;
}

/*
 * WinGetFuncArgInPartition
 *		Evaluate a window function's argument expression on a specified
 *		row of the partition.  The row is identified in lseek(2) style,
 *		i.e. relative to the current, first, or last row.
 *
 * argno: argument number to evaluate (counted from 0)
 * relpos: signed rowcount offset from the seek position
 * seektype: WINDOW_SEEK_CURRENT, WINDOW_SEEK_HEAD, or WINDOW_SEEK_TAIL
 * set_mark: If the row is found and set_mark is true, the mark is moved to
 *		the row as a side-effect.
 * isnull: output argument, receives isnull status of result
 * isout: output argument, set to indicate whether target row position
 *		is out of partition (can pass NULL if caller doesn't care about this)
 *
 * Specifying a nonexistent row is not an error, it just causes a null result
 * (plus setting *isout true, if isout isn't NULL).
 */
Datum
WinGetFuncArgInPartition(WindowObject winobj, int argno,
						 int relpos, int seektype, bool set_mark,
						 bool *isnull, bool *isout)
{
	WindowAggState *winstate;
	ExprContext *econtext;
	TupleTableSlot *slot;
	bool		gottuple;
	int64		abs_pos;

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;
	econtext = winstate->ss.ps.ps_ExprContext;
	slot = winstate->temp_slot_1;

	switch (seektype)
	{
		case WINDOW_SEEK_CURRENT:
			abs_pos = winstate->currentpos + relpos;
			break;
		case WINDOW_SEEK_HEAD:
			abs_pos = relpos;
			break;
		case WINDOW_SEEK_TAIL:
			spool_tuples(winstate, -1);
			abs_pos = winstate->spooled_rows - 1 + relpos;
			break;
		default:
			elog(ERROR, "unrecognized window seek type: %d", seektype);
			abs_pos = 0;		/* keep compiler quiet */
			break;
	}

	gottuple = window_gettupleslot(winobj, abs_pos, slot);

	if (!gottuple)
	{
		if (isout)
			*isout = true;
		*isnull = true;
		return (Datum) 0;
	}
	else
	{
		if (isout)
			*isout = false;
		if (set_mark)
		{
			int			frameOptions = winstate->frameOptions;
			int64		mark_pos = abs_pos;

			/*
			 * In RANGE mode with a moving frame head, we must not let the
			 * mark advance past frameheadpos, since that row has to be
			 * fetchable during future update_frameheadpos calls.
			 *
			 * XXX it is very ugly to pollute window functions' marks with
			 * this consideration; it could for instance mask a logic bug that
			 * lets a window function fetch rows before what it had claimed
			 * was its mark.  Perhaps use a separate mark for frame head
			 * probes?
			 */
			if ((frameOptions & FRAMEOPTION_RANGE) &&
				!(frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING))
			{
				update_frameheadpos(winobj, winstate->temp_slot_2);
				if (mark_pos > winstate->frameheadpos)
					mark_pos = winstate->frameheadpos;
			}
			WinSetMarkPosition(winobj, mark_pos);
		}
		econtext->ecxt_outertuple = slot;
		return ExecEvalExpr((ExprState *) list_nth(winobj->argstates, argno),
							econtext, isnull, NULL);
	}
}

/*
 * WinGetFuncArgInFrame
 *		Evaluate a window function's argument expression on a specified
 *		row of the window frame.  The row is identified in lseek(2) style,
 *		i.e. relative to the current, first, or last row.
 *
 * argno: argument number to evaluate (counted from 0)
 * relpos: signed rowcount offset from the seek position
 * seektype: WINDOW_SEEK_CURRENT, WINDOW_SEEK_HEAD, or WINDOW_SEEK_TAIL
 * set_mark: If the row is found and set_mark is true, the mark is moved to
 *		the row as a side-effect.
 * isnull: output argument, receives isnull status of result
 * isout: output argument, set to indicate whether target row position
 *		is out of frame (can pass NULL if caller doesn't care about this)
 *
 * Specifying a nonexistent row is not an error, it just causes a null result
 * (plus setting *isout true, if isout isn't NULL).
 */
Datum
WinGetFuncArgInFrame(WindowObject winobj, int argno,
					 int relpos, int seektype, bool set_mark,
					 bool *isnull, bool *isout)
{
	WindowAggState *winstate;
	ExprContext *econtext;
	TupleTableSlot *slot;
	bool		gottuple;
	int64		abs_pos;

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;
	econtext = winstate->ss.ps.ps_ExprContext;
	slot = winstate->temp_slot_1;

	switch (seektype)
	{
		case WINDOW_SEEK_CURRENT:
			abs_pos = winstate->currentpos + relpos;
			break;
		case WINDOW_SEEK_HEAD:
			update_frameheadpos(winobj, slot);
			abs_pos = winstate->frameheadpos + relpos;
			break;
		case WINDOW_SEEK_TAIL:
			update_frametailpos(winobj, slot);
			abs_pos = winstate->frametailpos + relpos;
			break;
		default:
			elog(ERROR, "unrecognized window seek type: %d", seektype);
			abs_pos = 0;		/* keep compiler quiet */
			break;
	}

	gottuple = window_gettupleslot(winobj, abs_pos, slot);
	if (gottuple)
		gottuple = row_is_in_frame(winstate, abs_pos, slot);

	if (!gottuple)
	{
		if (isout)
			*isout = true;
		*isnull = true;
		return (Datum) 0;
	}
	else
	{
		if (isout)
			*isout = false;
		if (set_mark)
		{
			int			frameOptions = winstate->frameOptions;
			int64		mark_pos = abs_pos;

			/*
			 * In RANGE mode with a moving frame head, we must not let the
			 * mark advance past frameheadpos, since that row has to be
			 * fetchable during future update_frameheadpos calls.
			 *
			 * XXX it is very ugly to pollute window functions' marks with
			 * this consideration; it could for instance mask a logic bug that
			 * lets a window function fetch rows before what it had claimed
			 * was its mark.  Perhaps use a separate mark for frame head
			 * probes?
			 */
			if ((frameOptions & FRAMEOPTION_RANGE) &&
				!(frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING))
			{
				update_frameheadpos(winobj, winstate->temp_slot_2);
				if (mark_pos > winstate->frameheadpos)
					mark_pos = winstate->frameheadpos;
			}
			WinSetMarkPosition(winobj, mark_pos);
		}
		econtext->ecxt_outertuple = slot;
		return ExecEvalExpr((ExprState *) list_nth(winobj->argstates, argno),
							econtext, isnull, NULL);
	}
}

/*
 * WinGetFuncArgCurrent
 *		Evaluate a window function's argument expression on the current row.
 *
 * argno: argument number to evaluate (counted from 0)
 * isnull: output argument, receives isnull status of result
 *
 * Note: this isn't quite equivalent to WinGetFuncArgInPartition or
 * WinGetFuncArgInFrame targeting the current row, because it will succeed
 * even if the WindowObject's mark has been set beyond the current row.
 * This should generally be used for "ordinary" arguments of a window
 * function, such as the offset argument of lead() or lag().
 */
Datum
WinGetFuncArgCurrent(WindowObject winobj, int argno, bool *isnull)
{
	WindowAggState *winstate;
	ExprContext *econtext;

	Assert(WindowObjectIsValid(winobj));
	winstate = winobj->winstate;

	econtext = winstate->ss.ps.ps_ExprContext;

	econtext->ecxt_outertuple = winstate->ss.ss_ScanTupleSlot;
	return ExecEvalExpr((ExprState *) list_nth(winobj->argstates, argno),
						econtext, isnull, NULL);
}
