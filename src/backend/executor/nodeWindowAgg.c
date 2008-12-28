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
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeWindowAgg.c,v 1.1 2008/12/28 18:53:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
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
	WindowFunc	   *wfunc;

	int			numArguments;	/* number of arguments */

	FmgrInfo	flinfo;			/* fmgr lookup data for window function */

	/*
	 * We need the len and byval info for the result of each function
	 * in order to know how to copy/delete values.
	 */
	int16		resulttypeLen;
	bool		resulttypeByVal;

	bool		plain_agg;		/* is it just a plain aggregate function? */
	int			aggno;			/* if so, index of its PerAggData */

	WindowObject	winobj;		/* object used in window function API */
} WindowStatePerFuncData;

/*
 * For plain aggregate window functions, we also have one of these.
 */
typedef struct WindowStatePerAggData
{
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
	 * initial value from pg_aggregate entry
	 */
	Datum		initValue;
	bool		initValueIsNull;

	/*
	 * cached value for non-moving frame
	 */
	Datum		resultValue;
	bool		resultValueIsNull;
	bool		hasResult;

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

	/* Current transition value */
	Datum		transValue;		/* current transition value */
	bool		transValueIsNull;

	bool		noTransValue;	/* true if transValue not set yet */
} WindowStatePerAggData;

static void initialize_windowaggregate(WindowAggState *winstate,
									   WindowStatePerFunc perfuncstate,
									   WindowStatePerAgg peraggstate);
static void advance_windowaggregate(WindowAggState *winstate,
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
 * parallel to initialize_aggregate in nodeAgg.c
 */
static void
initialize_windowaggregate(WindowAggState *winstate,
						   WindowStatePerFunc perfuncstate,
						   WindowStatePerAgg peraggstate)
{
	MemoryContext		oldContext;

	if (peraggstate->initValueIsNull)
		peraggstate->transValue = peraggstate->initValue;
	else
	{
		oldContext = MemoryContextSwitchTo(winstate->wincontext);
		peraggstate->transValue = datumCopy(peraggstate->initValue,
											peraggstate->transtypeByVal,
											peraggstate->transtypeLen);
		MemoryContextSwitchTo(oldContext);
	}
	peraggstate->transValueIsNull = peraggstate->initValueIsNull;
	peraggstate->noTransValue = peraggstate->initValueIsNull;
}

/*
 * advance_windowaggregate
 * parallel to advance_aggregate in nodeAgg.c
 */
static void
advance_windowaggregate(WindowAggState *winstate,
						WindowStatePerFunc perfuncstate,
						WindowStatePerAgg peraggstate)
{
	WindowFuncExprState	   *wfuncstate = perfuncstate->wfuncstate;
	int						numArguments = perfuncstate->numArguments;
	FunctionCallInfoData	fcinfodata;
	FunctionCallInfo		fcinfo = &fcinfodata;
	Datum					newVal;
	ListCell			   *arg;
	int						i;
	MemoryContext			oldContext;
	ExprContext *econtext = winstate->tmpcontext;

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/* We start from 1, since the 0th arg will be the transition value */
	i = 1;
	foreach(arg, wfuncstate->args)
	{
		ExprState	   *argstate = (ExprState *) lfirst(arg);

		fcinfo->arg[i] = ExecEvalExpr(argstate, econtext,
									  &fcinfo->argnull[i], NULL);
		i++;
	}

	if (peraggstate->transfn.fn_strict)
	{
		/*
		 * For a strict transfn, nothing happens when there's a NULL input; we
		 * just keep the prior transValue.
		 */
		for (i = 1; i <= numArguments; i++)
		{
			if (fcinfo->argnull[i])
			{
				MemoryContextSwitchTo(oldContext);
				return;
			}
		}
		if (peraggstate->noTransValue)
		{
			/*
			 * transValue has not been initialized. This is the first non-NULL
			 * input value. We use it as the initial value for transValue. (We
			 * already checked that the agg's input type is binary-compatible
			 * with its transtype, so straight copy here is OK.)
			 *
			 * We must copy the datum into wincontext if it is pass-by-ref. We
			 * do not need to pfree the old transValue, since it's NULL.
			 */
			MemoryContextSwitchTo(winstate->wincontext);
			peraggstate->transValue = datumCopy(fcinfo->arg[1],
											 peraggstate->transtypeByVal,
											 peraggstate->transtypeLen);
			peraggstate->transValueIsNull = false;
			peraggstate->noTransValue = false;
			MemoryContextSwitchTo(oldContext);
			return;
		}
		if (peraggstate->transValueIsNull)
		{
			/*
			 * Don't call a strict function with NULL inputs.  Note it is
			 * possible to get here despite the above tests, if the transfn is
			 * strict *and* returned a NULL on a prior cycle. If that happens
			 * we will propagate the NULL all the way to the end.
			 */
			MemoryContextSwitchTo(oldContext);
			return;
		}
	}

	/*
	 * OK to call the transition function
	 */
	InitFunctionCallInfoData(*fcinfo, &(peraggstate->transfn),
							 numArguments + 1,
							 (void *) winstate, NULL);
	fcinfo->arg[0] = peraggstate->transValue;
	fcinfo->argnull[0] = peraggstate->transValueIsNull;
	newVal = FunctionCallInvoke(fcinfo);

	/*
	 * If pass-by-ref datatype, must copy the new value into wincontext and
	 * pfree the prior transValue.	But if transfn returned a pointer to its
	 * first input, we don't need to do anything.
	 */
	if (!peraggstate->transtypeByVal &&
		DatumGetPointer(newVal) != DatumGetPointer(peraggstate->transValue))
	{
		if (!fcinfo->isnull)
		{
			MemoryContextSwitchTo(winstate->wincontext);
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
 * finalize_windowaggregate
 * parallel to finalize_aggregate in nodeAgg.c
 */
static void
finalize_windowaggregate(WindowAggState *winstate,
						 WindowStatePerFunc perfuncstate,
						 WindowStatePerAgg peraggstate,
						 Datum *result, bool *isnull)
{
	MemoryContext			oldContext;

	oldContext = MemoryContextSwitchTo(winstate->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

	/*
	 * Apply the agg's finalfn if one is provided, else return transValue.
	 */
	if (OidIsValid(peraggstate->finalfn_oid))
	{
		FunctionCallInfoData	fcinfo;

		InitFunctionCallInfoData(fcinfo, &(peraggstate->finalfn), 1,
								 (void *) winstate, NULL);
		fcinfo.arg[0] = peraggstate->transValue;
		fcinfo.argnull[0] = peraggstate->transValueIsNull;
		if (fcinfo.flinfo->fn_strict && peraggstate->transValueIsNull)
		{
			/* don't call a strict function with NULL inputs */
			*result = (Datum) 0;
			*isnull = true;
		}
		else
		{
			*result = FunctionCallInvoke(&fcinfo);
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
 * Much of this is duplicated from nodeAgg.c.  But NOTE that we expect to be
 * able to call aggregate final functions repeatedly after aggregating more
 * data onto the same transition value.  This is not a behavior required by
 * nodeAgg.c.
 */
static void
eval_windowaggregates(WindowAggState *winstate)
{
	WindowStatePerAgg   peraggstate;
	int					wfuncno, numaggs;
	int					i;
	MemoryContext		oldContext;
	ExprContext		   *econtext;
	TupleTableSlot	   *first_peer_slot = winstate->first_peer_slot;
	TupleTableSlot	   *slot;
	bool				first;

	numaggs = winstate->numaggs;
	if (numaggs == 0)
		return;					/* nothing to do */

	/* final output execution is in ps_ExprContext */
	econtext = winstate->ss.ps.ps_ExprContext;

	/*
	 * We don't currently support explicitly-specified window frames.  That
	 * means that the window frame always includes all the rows in the
	 * partition preceding and including the current row, and all its
	 * peers. As a special case, if there's no ORDER BY, all rows are peers,
	 * so the window frame includes all rows in the partition.
	 *
	 * When there's peer rows, all rows in a peer group will have the same
	 * aggregate values.  The values will be calculated when current position
	 * reaches the first peer row, and on all the following peer rows we will
	 * just return the saved results.
	 *
	 * 'aggregatedupto' keeps track of the last row that has already been
	 * accumulated for the aggregates. When the current row has no peers,
	 * aggregatedupto will be the same as the current row after this
	 * function. If there are peer rows, all peers will be accumulated in one
	 * call of this function, and aggregatedupto will be ahead of the current
	 * position. If there's no ORDER BY, and thus all rows are peers, the
	 * first call will aggregate all rows in the partition.
	 *
	 * TODO: In the future, we could implement sliding frames by recalculating
	 * the aggregate whenever a row exits the frame. That would be pretty
	 * slow, though. For aggregates like SUM and COUNT we could implement a
	 * "negative transition function" that would be called for all the rows
	 * that exit the frame.
	 */

	/*
	 * If we've already aggregated up through current row, reuse the
	 * saved result values
	 */
	if (winstate->aggregatedupto > winstate->currentpos)
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

	/* Initialize aggregates on first call for partition */
	for (i = 0; i < numaggs; i++)
	{
		peraggstate = &winstate->peragg[i];
		wfuncno = peraggstate->wfuncno;
		if (!peraggstate->hasResult)
			initialize_windowaggregate(winstate,
									   &winstate->perfunc[wfuncno],
									   &winstate->peragg[i]);
	}

	/*
	 * If this is the first call for this partition, fetch the first row
	 * for comparing peer rows. On subsequent calls, we'll always read
	 * ahead until we reach the first non-peer row, and store that row in
	 * first_peer_slot, for use in the next call.
	 */
	if (TupIsNull(first_peer_slot))
	{
		spool_tuples(winstate, winstate->aggregatedupto);
		tuplestore_select_read_pointer(winstate->buffer, winstate->agg_ptr);
		if (!tuplestore_gettupleslot(winstate->buffer, true, first_peer_slot))
			elog(ERROR, "unexpected end of tuplestore");
	}

	/*
	 * Advance until we reach the next non-peer row
	 */
	first = true;
	for (;;)
	{
		if (!first)
		{
			/* Fetch the next row, and see if it's a peer */
			spool_tuples(winstate, winstate->aggregatedupto);
			tuplestore_select_read_pointer(winstate->buffer,
										   winstate->agg_ptr);
			slot = winstate->temp_slot_1;
			if (!tuplestore_gettupleslot(winstate->buffer, true, slot))
				break;
			if (!are_peers(winstate, first_peer_slot, slot))
			{
				ExecCopySlot(first_peer_slot, slot);
				break;
			}
		}
		else
		{
			/*
			 * On first iteration, just accumulate the tuple saved from
			 * last call
			 */
			slot = first_peer_slot;
			first = false;
		}

		/* set tuple context for evaluation of aggregate arguments */
		winstate->tmpcontext->ecxt_outertuple = slot;

		for (i = 0; i < numaggs; i++)
		{
			wfuncno = winstate->peragg[i].wfuncno;

			advance_windowaggregate(winstate,
									&winstate->perfunc[wfuncno],
									&winstate->peragg[i]);

		}
		/* Reset per-input-tuple context after each tuple */
		ResetExprContext(winstate->tmpcontext);
		winstate->aggregatedupto++;
	}

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
								 peraggstate, result, isnull);

		/*
		 * save the result for the next (non-shrinking frame) call.
		 */
		if (!peraggstate->resulttypeByVal && !*isnull)
		{
			/*
			 * clear old resultValue in order not to leak memory.
			 */
			if (peraggstate->hasResult &&
				(DatumGetPointer(peraggstate->resultValue) !=
					DatumGetPointer(*result)) &&
				!peraggstate->resultValueIsNull)
				pfree(DatumGetPointer(peraggstate->resultValue));

			/*
			 * If pass-by-ref, copy it into our global context.
			 */
			oldContext = MemoryContextSwitchTo(winstate->wincontext);
			peraggstate->resultValue = datumCopy(*result,
												 peraggstate->resulttypeByVal,
												 peraggstate->resulttypeLen);
			MemoryContextSwitchTo(oldContext);
		}
		else
		{
			peraggstate->resultValue = *result;
		}
		peraggstate->resultValueIsNull = *isnull;
		peraggstate->hasResult = true;
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
	MemoryContext		oldContext;

	oldContext = MemoryContextSwitchTo(winstate->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

	/*
	 * We don't pass any normal arguments to a window function, but we do
	 * pass it the number of arguments, in order to permit window function
	 * implementations to support varying numbers of arguments.  The real
	 * info goes through the WindowObject, which is passed via fcinfo->context.
	 */
	InitFunctionCallInfoData(fcinfo, &(perfuncstate->flinfo),
							 perfuncstate->numArguments,
							 (void *) perfuncstate->winobj, NULL);
	/* Just in case, make all the regular argument slots be null */
	memset(fcinfo.argnull, true, perfuncstate->numArguments);

	*result = FunctionCallInvoke(&fcinfo);
	*isnull = fcinfo.isnull;

	/*
	 * Make sure pass-by-ref data is allocated in the appropriate context.
	 * (We need this in case the function returns a pointer into some
	 * short-lived tuple, as is entirely possible.)
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
	PlanState	   *outerPlan = outerPlanState(winstate);
	int				numfuncs = winstate->numfuncs;
	int				i;

	winstate->partition_spooled = false;
	winstate->spooled_rows = 0;
	winstate->currentpos = 0;
	winstate->frametailpos = -1;
	winstate->aggregatedupto = 0;

	/*
	 * If this is the very first partition, we need to fetch the first
	 * input row to store in it.
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
	 * Set up read pointers for the tuplestore.  The current and agg pointers
	 * don't need BACKWARD capability, but the per-window-function read
	 * pointers do.
	 */
	winstate->current_ptr = 0;	/* read pointer 0 is pre-allocated */

	/* reset default REWIND capability bit for current ptr */
	tuplestore_set_eflags(winstate->buffer, 0);

	/* create a read pointer for aggregates, if needed */
	if (winstate->numaggs > 0)
		winstate->agg_ptr = tuplestore_alloc_read_pointer(winstate->buffer, 0);

	/* create mark and read pointers for each real window function */
	for (i = 0; i < numfuncs; i++)
	{
		WindowStatePerFunc	perfuncstate = &(winstate->perfunc[i]);

		if (!perfuncstate->plain_agg)
		{
			WindowObject	winobj = perfuncstate->winobj;

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
 * Read tuples from the outer node, up to position 'pos', and store them
 * into the tuplestore. If pos is -1, reads the whole partition.
 */
static void
spool_tuples(WindowAggState *winstate, int64 pos)
{
	WindowAgg	   *node = (WindowAgg *) winstate->ss.ps.plan;
	PlanState	   *outerPlan;
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

	/* Must be in query context to call outerplan or touch tuplestore */
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
	int					i;

	for (i = 0; i < winstate->numfuncs; i++)
	{
		WindowStatePerFunc		perfuncstate = &(winstate->perfunc[i]);

		/* Release any partition-local state of this window function */
		if (perfuncstate->winobj)
			perfuncstate->winobj->localmem = NULL;

		/* Reset agg result cache */
		if (perfuncstate->plain_agg)
		{
			int		aggno = perfuncstate->aggno;
			WindowStatePerAggData *peraggstate = &winstate->peragg[aggno];

			peraggstate->resultValueIsNull = true;
			peraggstate->hasResult = false;
		}
	}

	/*
	 * Release all partition-local memory (in particular, any partition-local
	 * state or aggregate temp data that we might have trashed our pointers
	 * to in the above loop).  We don't rely on retail pfree because some
	 * aggregates might have allocated data we don't have direct pointers to.
	 */
	MemoryContextResetAndDeleteChildren(winstate->wincontext);

	/* Ensure eval_windowaggregates will see next call as partition start */
	ExecClearTuple(winstate->first_peer_slot);

	if (winstate->buffer)
		tuplestore_end(winstate->buffer);
	winstate->buffer = NULL;
	winstate->partition_spooled = false;
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
	ExprDoneCond	isDone;
	ExprContext	   *econtext;
	int				i;
	int				numfuncs;

	if (winstate->all_done)
		return NULL;

	/*
	 * Check to see if we're still projecting out tuples from a previous output
	 * tuple (because there is a function-returning-set in the projection
	 * expressions).  If so, try to project another one.
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

restart:
	if (winstate->buffer == NULL)
	{
		/* Initialize for first partition and set current row = 0 */
		begin_partition(winstate);
	}
	else
	{
		/* Advance current row within partition */
		winstate->currentpos++;
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
	 * Read the current row from the tuplestore, and save in ScanTupleSlot
	 * for possible use by WinGetFuncArgCurrent or the final projection step.
	 * (We can't rely on the outerplan's output slot because we may have to
	 * read beyond the current row.)
	 *
	 * Current row must be in the tuplestore, since we spooled it above.
	 */
	tuplestore_select_read_pointer(winstate->buffer, winstate->current_ptr);
	if (!tuplestore_gettupleslot(winstate->buffer, true,
								 winstate->ss.ss_ScanTupleSlot))
		elog(ERROR, "unexpected end of tuplestore");

	/*
	 * Evaluate true window functions
	 */
	numfuncs = winstate->numfuncs;
	for (i = 0; i < numfuncs; i++)
	{
		WindowStatePerFunc	perfuncstate = &(winstate->perfunc[i]);

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
	 * Form and return a projection tuple using the windowfunc results
	 * and the current row.  Setting ecxt_outertuple arranges that any
	 * Vars will be evaluated with respect to that row.
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
	WindowStatePerFunc  perfunc;
	WindowStatePerAgg   peragg;
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
	 * Create expression contexts.	We need two, one for per-input-tuple
	 * processing and one for per-output-tuple processing.	We cheat a little
	 * by using ExecAssignExprContext() to build both.
	 */
	ExecAssignExprContext(estate, &winstate->ss.ps);
	tmpcontext = winstate->ss.ps.ps_ExprContext;
	winstate->tmpcontext = tmpcontext;
	ExecAssignExprContext(estate, &winstate->ss.ps);

	/* Create long-lived context for storage of aggregate transvalues etc */
	winstate->wincontext =
		AllocSetContextCreate(CurrentMemoryContext,
							  "WindowAggContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

#define WINDOWAGG_NSLOTS 6

	/*
	 * tuple table initialization
	 */
	ExecInitScanTupleSlot(estate, &winstate->ss);
	ExecInitResultTupleSlot(estate, &winstate->ss.ps);
	winstate->first_part_slot = ExecInitExtraTupleSlot(estate);
	winstate->first_peer_slot = ExecInitExtraTupleSlot(estate);
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
	ExecSetSlotDescriptor(winstate->first_peer_slot,
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
		WindowFuncExprState	   *wfuncstate = (WindowFuncExprState *) lfirst(l);
		WindowFunc			   *wfunc = (WindowFunc *) wfuncstate->xprstate.expr;
		WindowStatePerFunc perfuncstate;
		AclResult	aclresult;
		int			i;

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

		/* Fill in the perfuncstate data */
		perfuncstate->wfuncstate = wfuncstate;
		perfuncstate->wfunc = wfunc;
		perfuncstate->numArguments = list_length(wfuncstate->args);

		fmgr_info_cxt(wfunc->winfnoid, &perfuncstate->flinfo,
					  tmpcontext->ecxt_per_query_memory);
		perfuncstate->flinfo.fn_expr = (Node *) wfunc;
		get_typlenbyval(wfunc->wintype,
						&perfuncstate->resulttypeLen,
						&perfuncstate->resulttypeByVal);

		/*
		 * If it's really just a plain aggregate function,
		 * we'll emulate the Agg environment for it.
		 */
		perfuncstate->plain_agg = wfunc->winagg;
		if (wfunc->winagg)
		{
			WindowStatePerAgg	peraggstate;

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

	winstate->partition_spooled = false;
	winstate->more_partitions = false;

	return winstate;
}

/* -----------------
 * ExecCountSlotsWindowAgg
 * -----------------
 */
int
ExecCountSlotsWindowAgg(WindowAgg *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
		ExecCountSlotsNode(innerPlan(node)) +
		WINDOWAGG_NSLOTS;
}

/* -----------------
 * ExecEndWindowAgg
 * -----------------
 */
void
ExecEndWindowAgg(WindowAggState *node)
{
	PlanState  *outerPlan;

	release_partition(node);

	pfree(node->perfunc);
	pfree(node->peragg);

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	ExecClearTuple(node->first_part_slot);
	ExecClearTuple(node->first_peer_slot);
	ExecClearTuple(node->temp_slot_1);
	ExecClearTuple(node->temp_slot_2);

	/*
	 * Free both the expr contexts.
	 */
	ExecFreeExprContext(&node->ss.ps);
	node->ss.ps.ps_ExprContext = node->tmpcontext;
	ExecFreeExprContext(&node->ss.ps);

	MemoryContextDelete(node->wincontext);

	outerPlan = outerPlanState(node);
	ExecEndNode(outerPlan);
}

/* -----------------
 * ExecRescanWindowAgg
 * -----------------
 */
void
ExecReScanWindowAgg(WindowAggState *node, ExprContext *exprCtxt)
{
	ExprContext	   *econtext = node->ss.ps.ps_ExprContext;

	node->all_done = false;

	node->ss.ps.ps_TupFromTlist = false;

	/* release tuplestore et al */
	release_partition(node);

	/* release all temp tuples, but especially first_part_slot */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	ExecClearTuple(node->first_part_slot);
	ExecClearTuple(node->first_peer_slot);
	ExecClearTuple(node->temp_slot_1);
	ExecClearTuple(node->temp_slot_2);

	/* Forget current wfunc values */
	MemSet(econtext->ecxt_aggvalues, 0, sizeof(Datum) * node->numfuncs);
	MemSet(econtext->ecxt_aggnulls, 0, sizeof(bool) * node->numfuncs);

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((PlanState *) node)->lefttree->chgParam == NULL)
		ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
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
	AclResult	aclresult;
	Oid			transfn_oid,
				finalfn_oid;
	Expr	   *transfnexpr,
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

	aggTuple = SearchSysCache(AGGFNOID,
							  ObjectIdGetDatum(wfunc->winfnoid),
							  0, 0, 0);
	if (!HeapTupleIsValid(aggTuple))
		elog(ERROR, "cache lookup failed for aggregate %u",
			 wfunc->winfnoid);
	aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);

	/*
	 * ExecInitWindowAgg already checked permission to call aggregate function
	 * ... but we still need to check the component functions
	 */

	peraggstate->transfn_oid = transfn_oid = aggform->aggtransfn;
	peraggstate->finalfn_oid = finalfn_oid = aggform->aggfinalfn;

	/* Check that aggregate owner has permission to call component fns */
	{
		HeapTuple	procTuple;
		Oid			aggOwner;

		procTuple = SearchSysCache(PROCOID,
								   ObjectIdGetDatum(wfunc->winfnoid),
								   0, 0, 0);
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

		get_func_signature(wfunc->winfnoid,
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
							wfunc->wintype,
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
							wfunc->winfnoid)));
	}

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
 *	Fetch the pos'th tuple of the current partition into the slot
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
	 * There's no API to refetch the tuple at the current position. We
	 * have to move one tuple forward, and then one backward.  (We don't
	 * do it the other way because we might try to fetch the row before
	 * our mark, which isn't allowed.)
	 */
	if (winobj->seekpos == pos)
	{
		tuplestore_advance(winstate->buffer, true);
		winobj->seekpos++;
	}

	while (winobj->seekpos > pos)
	{
		if (!tuplestore_gettupleslot(winstate->buffer, false, slot))
			elog(ERROR, "unexpected end of tuplestore");
		winobj->seekpos--;
	}

	while (winobj->seekpos < pos)
	{
		if (!tuplestore_gettupleslot(winstate->buffer, true, slot))
			elog(ERROR, "unexpected end of tuplestore");
		winobj->seekpos++;
	}

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
		winobj->localmem = MemoryContextAllocZero(winobj->winstate->wincontext,
												  sz);
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
	while (markpos > winobj->markpos)
	{
		tuplestore_advance(winstate->buffer, true);
		winobj->markpos++;
	}
	tuplestore_select_read_pointer(winstate->buffer, winobj->readptr);
	while (markpos > winobj->seekpos)
	{
		tuplestore_advance(winstate->buffer, true);
		winobj->seekpos++;
	}
}

/*
 * WinRowsArePeers
 *		Compare two rows (specified by absolute position in window) to see
 *		if they are equal according to the ORDER BY clause.
 */
bool
WinRowsArePeers(WindowObject winobj, int64 pos1, int64 pos2)
{
	WindowAggState *winstate;
	WindowAgg	   *node;
	TupleTableSlot *slot1;
	TupleTableSlot *slot2;
	bool			res;

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
	ExprContext *econtext;
	TupleTableSlot *slot;
	bool		gottuple;
	int64		abs_pos;

	Assert(WindowObjectIsValid(winobj));

	econtext = winobj->winstate->ss.ps.ps_ExprContext;
	slot = winobj->winstate->temp_slot_1;

	switch (seektype)
	{
		case WINDOW_SEEK_CURRENT:
			abs_pos = winobj->winstate->currentpos + relpos;
			break;
		case WINDOW_SEEK_HEAD:
			abs_pos = relpos;
			break;
		case WINDOW_SEEK_TAIL:
			spool_tuples(winobj->winstate, -1);
			abs_pos = winobj->winstate->spooled_rows - 1 + relpos;
			break;
		default:
			elog(ERROR, "unrecognized window seek type: %d", seektype);
			abs_pos = 0; /* keep compiler quiet */
			break;
	}

	if (abs_pos >= 0)
		gottuple = window_gettupleslot(winobj, abs_pos, slot);
	else
		gottuple = false;

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
			WinSetMarkPosition(winobj, abs_pos);
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
	ExprContext *econtext;
	TupleTableSlot *slot;
	bool		gottuple;
	int64		abs_pos;
	int64		frametailpos;

	Assert(WindowObjectIsValid(winobj));

	/* if no ordering columns, partition and frame are the same thing */
	if (((WindowAgg *) winobj->winstate->ss.ps.plan)->ordNumCols == 0)
		return WinGetFuncArgInPartition(winobj, argno, relpos, seektype,
										set_mark, isnull, isout);

	econtext = winobj->winstate->ss.ps.ps_ExprContext;
	slot = winobj->winstate->temp_slot_1;
	frametailpos = winobj->winstate->frametailpos;

	switch (seektype)
	{
		case WINDOW_SEEK_CURRENT:
			abs_pos = winobj->winstate->currentpos + relpos;
			break;
		case WINDOW_SEEK_HEAD:
			abs_pos = relpos;
			break;
		case WINDOW_SEEK_TAIL:
			/* abs_pos is calculated later */
			abs_pos = 0; /* keep compiler quiet */
			break;
		default:
			elog(ERROR, "unrecognized window seek type: %d", seektype);
			abs_pos = 0; /* keep compiler quiet */
			break;
	}

	/*
	 * Seek for frame tail. If the tail position is before current,
	 * always check if the tail is after the current or not.
	 */
	if (frametailpos <= winobj->winstate->currentpos)
	{
		int64 add = 1;

		for (;;)
		{
			spool_tuples(winobj->winstate, winobj->winstate->currentpos + add);
			if (winobj->winstate->spooled_rows > winobj->winstate->currentpos + add)
			{
				/*
				 * When seektype is not TAIL, we may optimize not to
				 * spool unnecessary tuples. In TAIL mode, we need to search
				 * until we find a row that's definitely not a peer.
				 */
				if (!WinRowsArePeers(winobj, winobj->winstate->currentpos,
									 winobj->winstate->currentpos + add) ||
					(seektype != WINDOW_SEEK_TAIL &&
					 winobj->winstate->currentpos + add < abs_pos))
					break;
				add++;
			}
			else
			{
				/*
				 * If hit the partition end, the last row is the frame tail.
				 */
				break;
			}
		}
		frametailpos = winobj->winstate->currentpos + add - 1;
		winobj->winstate->frametailpos = frametailpos;
	}

	if (seektype == WINDOW_SEEK_TAIL)
	{
		abs_pos = frametailpos + relpos;
	}

	/*
	 * If there is an ORDER BY (we don't support other window frame
	 * specifications yet), the frame runs from first row of the partition
	 * to the last peer of the current row. Otherwise the frame is the
	 * whole partition.
	 */
	if (abs_pos < 0 || abs_pos > frametailpos)
		gottuple = false;
	else
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
			WinSetMarkPosition(winobj, abs_pos);
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
