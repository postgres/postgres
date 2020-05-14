/*-------------------------------------------------------------------------
 *
 * execSRF.c
 *	  Routines implementing the API for set-returning functions
 *
 * This file serves nodeFunctionscan.c and nodeProjectSet.c, providing
 * common code for calling set-returning functions according to the
 * ReturnSetInfo API.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execSRF.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/objectaccess.h"
#include "executor/execdebug.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "pgstat.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/typcache.h"


/* static function decls */
static void init_sexpr(Oid foid, Oid input_collation, Expr *node,
					   SetExprState *sexpr, PlanState *parent,
					   MemoryContext sexprCxt, bool allowSRF, bool needDescForSRF);
static void ShutdownSetExpr(Datum arg);
static void ExecEvalFuncArgs(FunctionCallInfo fcinfo,
							 List *argList, ExprContext *econtext);
static void ExecPrepareTuplestoreResult(SetExprState *sexpr,
										ExprContext *econtext,
										Tuplestorestate *resultStore,
										TupleDesc resultDesc);
static void tupledesc_match(TupleDesc dst_tupdesc, TupleDesc src_tupdesc);


/*
 * Prepare function call in FROM (ROWS FROM) for execution.
 *
 * This is used by nodeFunctionscan.c.
 */
SetExprState *
ExecInitTableFunctionResult(Expr *expr,
							ExprContext *econtext, PlanState *parent)
{
	SetExprState *state = makeNode(SetExprState);

	state->funcReturnsSet = false;
	state->expr = expr;
	state->func.fn_oid = InvalidOid;

	/*
	 * Normally the passed expression tree will be a FuncExpr, since the
	 * grammar only allows a function call at the top level of a table
	 * function reference.  However, if the function doesn't return set then
	 * the planner might have replaced the function call via constant-folding
	 * or inlining.  So if we see any other kind of expression node, execute
	 * it via the general ExecEvalExpr() code.  That code path will not
	 * support set-returning functions buried in the expression, though.
	 */
	if (IsA(expr, FuncExpr))
	{
		FuncExpr   *func = (FuncExpr *) expr;

		state->funcReturnsSet = func->funcretset;
		state->args = ExecInitExprList(func->args, parent);

		init_sexpr(func->funcid, func->inputcollid, expr, state, parent,
				   econtext->ecxt_per_query_memory, func->funcretset, false);
	}
	else
	{
		state->elidedFuncState = ExecInitExpr(expr, parent);
	}

	return state;
}

/*
 *		ExecMakeTableFunctionResult
 *
 * Evaluate a table function, producing a materialized result in a Tuplestore
 * object.
 *
 * This is used by nodeFunctionscan.c.
 */
Tuplestorestate *
ExecMakeTableFunctionResult(SetExprState *setexpr,
							ExprContext *econtext,
							MemoryContext argContext,
							TupleDesc expectedDesc,
							bool randomAccess)
{
	Tuplestorestate *tupstore = NULL;
	TupleDesc	tupdesc = NULL;
	Oid			funcrettype;
	bool		returnsTuple;
	bool		returnsSet = false;
	FunctionCallInfo fcinfo;
	PgStat_FunctionCallUsage fcusage;
	ReturnSetInfo rsinfo;
	HeapTupleData tmptup;
	MemoryContext callerContext;
	bool		first_time = true;

	/*
	 * Execute per-tablefunc actions in appropriate context.
	 *
	 * The FunctionCallInfo needs to live across all the calls to a
	 * ValuePerCall function, so it can't be allocated in the per-tuple
	 * context. Similarly, the function arguments need to be evaluated in a
	 * context that is longer lived than the per-tuple context: The argument
	 * values would otherwise disappear when we reset that context in the
	 * inner loop.  As the caller's CurrentMemoryContext is typically a
	 * query-lifespan context, we don't want to leak memory there.  We require
	 * the caller to pass a separate memory context that can be used for this,
	 * and can be reset each time through to avoid bloat.
	 */
	MemoryContextReset(argContext);
	callerContext = MemoryContextSwitchTo(argContext);

	funcrettype = exprType((Node *) setexpr->expr);

	returnsTuple = type_is_rowtype(funcrettype);

	/*
	 * Prepare a resultinfo node for communication.  We always do this even if
	 * not expecting a set result, so that we can pass expectedDesc.  In the
	 * generic-expression case, the expression doesn't actually get to see the
	 * resultinfo, but set it up anyway because we use some of the fields as
	 * our own state variables.
	 */
	rsinfo.type = T_ReturnSetInfo;
	rsinfo.econtext = econtext;
	rsinfo.expectedDesc = expectedDesc;
	rsinfo.allowedModes = (int) (SFRM_ValuePerCall | SFRM_Materialize | SFRM_Materialize_Preferred);
	if (randomAccess)
		rsinfo.allowedModes |= (int) SFRM_Materialize_Random;
	rsinfo.returnMode = SFRM_ValuePerCall;
	/* isDone is filled below */
	rsinfo.setResult = NULL;
	rsinfo.setDesc = NULL;

	fcinfo = palloc(SizeForFunctionCallInfo(list_length(setexpr->args)));

	/*
	 * Normally the passed expression tree will be a SetExprState, since the
	 * grammar only allows a function call at the top level of a table
	 * function reference.  However, if the function doesn't return set then
	 * the planner might have replaced the function call via constant-folding
	 * or inlining.  So if we see any other kind of expression node, execute
	 * it via the general ExecEvalExpr() code; the only difference is that we
	 * don't get a chance to pass a special ReturnSetInfo to any functions
	 * buried in the expression.
	 */
	if (!setexpr->elidedFuncState)
	{
		/*
		 * This path is similar to ExecMakeFunctionResultSet.
		 */
		returnsSet = setexpr->funcReturnsSet;
		InitFunctionCallInfoData(*fcinfo, &(setexpr->func),
								 list_length(setexpr->args),
								 setexpr->fcinfo->fncollation,
								 NULL, (Node *) &rsinfo);
		/* evaluate the function's argument list */
		Assert(CurrentMemoryContext == argContext);
		ExecEvalFuncArgs(fcinfo, setexpr->args, econtext);

		/*
		 * If function is strict, and there are any NULL arguments, skip
		 * calling the function and act like it returned NULL (or an empty
		 * set, in the returns-set case).
		 */
		if (setexpr->func.fn_strict)
		{
			int			i;

			for (i = 0; i < fcinfo->nargs; i++)
			{
				if (fcinfo->args[i].isnull)
					goto no_function_result;
			}
		}
	}
	else
	{
		/* Treat setexpr as a generic expression */
		InitFunctionCallInfoData(*fcinfo, NULL, 0, InvalidOid, NULL, NULL);
	}

	/*
	 * Switch to short-lived context for calling the function or expression.
	 */
	MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/*
	 * Loop to handle the ValuePerCall protocol (which is also the same
	 * behavior needed in the generic ExecEvalExpr path).
	 */
	for (;;)
	{
		Datum		result;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Reset per-tuple memory context before each call of the function or
		 * expression. This cleans up any local memory the function may leak
		 * when called.
		 */
		ResetExprContext(econtext);

		/* Call the function or expression one time */
		if (!setexpr->elidedFuncState)
		{
			pgstat_init_function_usage(fcinfo, &fcusage);

			fcinfo->isnull = false;
			rsinfo.isDone = ExprSingleResult;
			result = FunctionCallInvoke(fcinfo);

			pgstat_end_function_usage(&fcusage,
									  rsinfo.isDone != ExprMultipleResult);
		}
		else
		{
			result =
				ExecEvalExpr(setexpr->elidedFuncState, econtext, &fcinfo->isnull);
			rsinfo.isDone = ExprSingleResult;
		}

		/* Which protocol does function want to use? */
		if (rsinfo.returnMode == SFRM_ValuePerCall)
		{
			/*
			 * Check for end of result set.
			 */
			if (rsinfo.isDone == ExprEndResult)
				break;

			/*
			 * If first time through, build tuplestore for result.  For a
			 * scalar function result type, also make a suitable tupdesc.
			 */
			if (first_time)
			{
				MemoryContext oldcontext =
				MemoryContextSwitchTo(econtext->ecxt_per_query_memory);

				tupstore = tuplestore_begin_heap(randomAccess, false, work_mem);
				rsinfo.setResult = tupstore;
				if (!returnsTuple)
				{
					tupdesc = CreateTemplateTupleDesc(1);
					TupleDescInitEntry(tupdesc,
									   (AttrNumber) 1,
									   "column",
									   funcrettype,
									   -1,
									   0);
					rsinfo.setDesc = tupdesc;
				}
				MemoryContextSwitchTo(oldcontext);
			}

			/*
			 * Store current resultset item.
			 */
			if (returnsTuple)
			{
				if (!fcinfo->isnull)
				{
					HeapTupleHeader td = DatumGetHeapTupleHeader(result);

					if (tupdesc == NULL)
					{
						MemoryContext oldcontext =
						MemoryContextSwitchTo(econtext->ecxt_per_query_memory);

						/*
						 * This is the first non-NULL result from the
						 * function.  Use the type info embedded in the
						 * rowtype Datum to look up the needed tupdesc.  Make
						 * a copy for the query.
						 */
						tupdesc = lookup_rowtype_tupdesc_copy(HeapTupleHeaderGetTypeId(td),
															  HeapTupleHeaderGetTypMod(td));
						rsinfo.setDesc = tupdesc;
						MemoryContextSwitchTo(oldcontext);
					}
					else
					{
						/*
						 * Verify all later returned rows have same subtype;
						 * necessary in case the type is RECORD.
						 */
						if (HeapTupleHeaderGetTypeId(td) != tupdesc->tdtypeid ||
							HeapTupleHeaderGetTypMod(td) != tupdesc->tdtypmod)
							ereport(ERROR,
									(errcode(ERRCODE_DATATYPE_MISMATCH),
									 errmsg("rows returned by function are not all of the same row type")));
					}

					/*
					 * tuplestore_puttuple needs a HeapTuple not a bare
					 * HeapTupleHeader, but it doesn't need all the fields.
					 */
					tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
					tmptup.t_data = td;

					tuplestore_puttuple(tupstore, &tmptup);
				}
				else
				{
					/*
					 * NULL result from a tuple-returning function; expand it
					 * to a row of all nulls.  We rely on the expectedDesc to
					 * form such rows.  (Note: this would be problematic if
					 * tuplestore_putvalues saved the tdtypeid/tdtypmod from
					 * the provided descriptor, since that might not match
					 * what we get from the function itself.  But it doesn't.)
					 */
					int			natts = expectedDesc->natts;
					bool	   *nullflags;

					nullflags = (bool *) palloc(natts * sizeof(bool));
					memset(nullflags, true, natts * sizeof(bool));
					tuplestore_putvalues(tupstore, expectedDesc, NULL, nullflags);
				}
			}
			else
			{
				/* Scalar-type case: just store the function result */
				tuplestore_putvalues(tupstore, tupdesc, &result, &fcinfo->isnull);
			}

			/*
			 * Are we done?
			 */
			if (rsinfo.isDone != ExprMultipleResult)
				break;
		}
		else if (rsinfo.returnMode == SFRM_Materialize)
		{
			/* check we're on the same page as the function author */
			if (!first_time || rsinfo.isDone != ExprSingleResult)
				ereport(ERROR,
						(errcode(ERRCODE_E_R_I_E_SRF_PROTOCOL_VIOLATED),
						 errmsg("table-function protocol for materialize mode was not followed")));
			/* Done evaluating the set result */
			break;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_SRF_PROTOCOL_VIOLATED),
					 errmsg("unrecognized table-function returnMode: %d",
							(int) rsinfo.returnMode)));

		first_time = false;
	}

no_function_result:

	/*
	 * If we got nothing from the function (ie, an empty-set or NULL result),
	 * we have to create the tuplestore to return, and if it's a
	 * non-set-returning function then insert a single all-nulls row.  As
	 * above, we depend on the expectedDesc to manufacture the dummy row.
	 */
	if (rsinfo.setResult == NULL)
	{
		MemoryContext oldcontext =
		MemoryContextSwitchTo(econtext->ecxt_per_query_memory);

		tupstore = tuplestore_begin_heap(randomAccess, false, work_mem);
		rsinfo.setResult = tupstore;
		MemoryContextSwitchTo(oldcontext);

		if (!returnsSet)
		{
			int			natts = expectedDesc->natts;
			bool	   *nullflags;

			nullflags = (bool *) palloc(natts * sizeof(bool));
			memset(nullflags, true, natts * sizeof(bool));
			tuplestore_putvalues(tupstore, expectedDesc, NULL, nullflags);
		}
	}

	/*
	 * If function provided a tupdesc, cross-check it.  We only really need to
	 * do this for functions returning RECORD, but might as well do it always.
	 */
	if (rsinfo.setDesc)
	{
		tupledesc_match(expectedDesc, rsinfo.setDesc);

		/*
		 * If it is a dynamically-allocated TupleDesc, free it: it is
		 * typically allocated in a per-query context, so we must avoid
		 * leaking it across multiple usages.
		 */
		if (rsinfo.setDesc->tdrefcount == -1)
			FreeTupleDesc(rsinfo.setDesc);
	}

	MemoryContextSwitchTo(callerContext);

	/* All done, pass back the tuplestore */
	return rsinfo.setResult;
}


/*
 * Prepare targetlist SRF function call for execution.
 *
 * This is used by nodeProjectSet.c.
 */
SetExprState *
ExecInitFunctionResultSet(Expr *expr,
						  ExprContext *econtext, PlanState *parent)
{
	SetExprState *state = makeNode(SetExprState);

	state->funcReturnsSet = true;
	state->expr = expr;
	state->func.fn_oid = InvalidOid;

	/*
	 * Initialize metadata.  The expression node could be either a FuncExpr or
	 * an OpExpr.
	 */
	if (IsA(expr, FuncExpr))
	{
		FuncExpr   *func = (FuncExpr *) expr;

		state->args = ExecInitExprList(func->args, parent);
		init_sexpr(func->funcid, func->inputcollid, expr, state, parent,
				   econtext->ecxt_per_query_memory, true, true);
	}
	else if (IsA(expr, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) expr;

		state->args = ExecInitExprList(op->args, parent);
		init_sexpr(op->opfuncid, op->inputcollid, expr, state, parent,
				   econtext->ecxt_per_query_memory, true, true);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(expr));

	/* shouldn't get here unless the selected function returns set */
	Assert(state->func.fn_retset);

	return state;
}

/*
 *		ExecMakeFunctionResultSet
 *
 * Evaluate the arguments to a set-returning function and then call the
 * function itself.  The argument expressions may not contain set-returning
 * functions (the planner is supposed to have separated evaluation for those).
 *
 * This should be called in a short-lived (per-tuple) context, argContext
 * needs to live until all rows have been returned (i.e. *isDone set to
 * ExprEndResult or ExprSingleResult).
 *
 * This is used by nodeProjectSet.c.
 */
Datum
ExecMakeFunctionResultSet(SetExprState *fcache,
						  ExprContext *econtext,
						  MemoryContext argContext,
						  bool *isNull,
						  ExprDoneCond *isDone)
{
	List	   *arguments;
	Datum		result;
	FunctionCallInfo fcinfo;
	PgStat_FunctionCallUsage fcusage;
	ReturnSetInfo rsinfo;
	bool		callit;
	int			i;

restart:

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	/*
	 * If a previous call of the function returned a set result in the form of
	 * a tuplestore, continue reading rows from the tuplestore until it's
	 * empty.
	 */
	if (fcache->funcResultStore)
	{
		TupleTableSlot *slot = fcache->funcResultSlot;
		MemoryContext oldContext;
		bool		foundTup;

		/*
		 * Have to make sure tuple in slot lives long enough, otherwise
		 * clearing the slot could end up trying to free something already
		 * freed.
		 */
		oldContext = MemoryContextSwitchTo(slot->tts_mcxt);
		foundTup = tuplestore_gettupleslot(fcache->funcResultStore, true, false,
										   fcache->funcResultSlot);
		MemoryContextSwitchTo(oldContext);

		if (foundTup)
		{
			*isDone = ExprMultipleResult;
			if (fcache->funcReturnsTuple)
			{
				/* We must return the whole tuple as a Datum. */
				*isNull = false;
				return ExecFetchSlotHeapTupleDatum(fcache->funcResultSlot);
			}
			else
			{
				/* Extract the first column and return it as a scalar. */
				return slot_getattr(fcache->funcResultSlot, 1, isNull);
			}
		}
		/* Exhausted the tuplestore, so clean up */
		tuplestore_end(fcache->funcResultStore);
		fcache->funcResultStore = NULL;
		*isDone = ExprEndResult;
		*isNull = true;
		return (Datum) 0;
	}

	/*
	 * arguments is a list of expressions to evaluate before passing to the
	 * function manager.  We skip the evaluation if it was already done in the
	 * previous call (ie, we are continuing the evaluation of a set-valued
	 * function).  Otherwise, collect the current argument values into fcinfo.
	 *
	 * The arguments have to live in a context that lives at least until all
	 * rows from this SRF have been returned, otherwise ValuePerCall SRFs
	 * would reference freed memory after the first returned row.
	 */
	fcinfo = fcache->fcinfo;
	arguments = fcache->args;
	if (!fcache->setArgsValid)
	{
		MemoryContext oldContext = MemoryContextSwitchTo(argContext);

		ExecEvalFuncArgs(fcinfo, arguments, econtext);
		MemoryContextSwitchTo(oldContext);
	}
	else
	{
		/* Reset flag (we may set it again below) */
		fcache->setArgsValid = false;
	}

	/*
	 * Now call the function, passing the evaluated parameter values.
	 */

	/* Prepare a resultinfo node for communication. */
	fcinfo->resultinfo = (Node *) &rsinfo;
	rsinfo.type = T_ReturnSetInfo;
	rsinfo.econtext = econtext;
	rsinfo.expectedDesc = fcache->funcResultDesc;
	rsinfo.allowedModes = (int) (SFRM_ValuePerCall | SFRM_Materialize);
	/* note we do not set SFRM_Materialize_Random or _Preferred */
	rsinfo.returnMode = SFRM_ValuePerCall;
	/* isDone is filled below */
	rsinfo.setResult = NULL;
	rsinfo.setDesc = NULL;

	/*
	 * If function is strict, and there are any NULL arguments, skip calling
	 * the function.
	 */
	callit = true;
	if (fcache->func.fn_strict)
	{
		for (i = 0; i < fcinfo->nargs; i++)
		{
			if (fcinfo->args[i].isnull)
			{
				callit = false;
				break;
			}
		}
	}

	if (callit)
	{
		pgstat_init_function_usage(fcinfo, &fcusage);

		fcinfo->isnull = false;
		rsinfo.isDone = ExprSingleResult;
		result = FunctionCallInvoke(fcinfo);
		*isNull = fcinfo->isnull;
		*isDone = rsinfo.isDone;

		pgstat_end_function_usage(&fcusage,
								  rsinfo.isDone != ExprMultipleResult);
	}
	else
	{
		/* for a strict SRF, result for NULL is an empty set */
		result = (Datum) 0;
		*isNull = true;
		*isDone = ExprEndResult;
	}

	/* Which protocol does function want to use? */
	if (rsinfo.returnMode == SFRM_ValuePerCall)
	{
		if (*isDone != ExprEndResult)
		{
			/*
			 * Save the current argument values to re-use on the next call.
			 */
			if (*isDone == ExprMultipleResult)
			{
				fcache->setArgsValid = true;
				/* Register cleanup callback if we didn't already */
				if (!fcache->shutdown_reg)
				{
					RegisterExprContextCallback(econtext,
												ShutdownSetExpr,
												PointerGetDatum(fcache));
					fcache->shutdown_reg = true;
				}
			}
		}
	}
	else if (rsinfo.returnMode == SFRM_Materialize)
	{
		/* check we're on the same page as the function author */
		if (rsinfo.isDone != ExprSingleResult)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_SRF_PROTOCOL_VIOLATED),
					 errmsg("table-function protocol for materialize mode was not followed")));
		if (rsinfo.setResult != NULL)
		{
			/* prepare to return values from the tuplestore */
			ExecPrepareTuplestoreResult(fcache, econtext,
										rsinfo.setResult,
										rsinfo.setDesc);
			/* loop back to top to start returning from tuplestore */
			goto restart;
		}
		/* if setResult was left null, treat it as empty set */
		*isDone = ExprEndResult;
		*isNull = true;
		result = (Datum) 0;
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_SRF_PROTOCOL_VIOLATED),
				 errmsg("unrecognized table-function returnMode: %d",
						(int) rsinfo.returnMode)));

	return result;
}


/*
 * init_sexpr - initialize a SetExprState node during first use
 */
static void
init_sexpr(Oid foid, Oid input_collation, Expr *node,
		   SetExprState *sexpr, PlanState *parent,
		   MemoryContext sexprCxt, bool allowSRF, bool needDescForSRF)
{
	AclResult	aclresult;
	size_t		numargs = list_length(sexpr->args);

	/* Check permission to call function */
	aclresult = pg_proc_aclcheck(foid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(foid));
	InvokeFunctionExecuteHook(foid);

	/*
	 * Safety check on nargs.  Under normal circumstances this should never
	 * fail, as parser should check sooner.  But possibly it might fail if
	 * server has been compiled with FUNC_MAX_ARGS smaller than some functions
	 * declared in pg_proc?
	 */
	if (list_length(sexpr->args) > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg_plural("cannot pass more than %d argument to a function",
							   "cannot pass more than %d arguments to a function",
							   FUNC_MAX_ARGS,
							   FUNC_MAX_ARGS)));

	/* Set up the primary fmgr lookup information */
	fmgr_info_cxt(foid, &(sexpr->func), sexprCxt);
	fmgr_info_set_expr((Node *) sexpr->expr, &(sexpr->func));

	/* Initialize the function call parameter struct as well */
	sexpr->fcinfo =
		(FunctionCallInfo) palloc(SizeForFunctionCallInfo(numargs));
	InitFunctionCallInfoData(*sexpr->fcinfo, &(sexpr->func),
							 numargs,
							 input_collation, NULL, NULL);

	/* If function returns set, check if that's allowed by caller */
	if (sexpr->func.fn_retset && !allowSRF)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set"),
				 parent ? executor_errposition(parent->state,
											   exprLocation((Node *) node)) : 0));

	/* Otherwise, caller should have marked the sexpr correctly */
	Assert(sexpr->func.fn_retset == sexpr->funcReturnsSet);

	/* If function returns set, prepare expected tuple descriptor */
	if (sexpr->func.fn_retset && needDescForSRF)
	{
		TypeFuncClass functypclass;
		Oid			funcrettype;
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		functypclass = get_expr_result_type(sexpr->func.fn_expr,
											&funcrettype,
											&tupdesc);

		/* Must save tupdesc in sexpr's context */
		oldcontext = MemoryContextSwitchTo(sexprCxt);

		if (functypclass == TYPEFUNC_COMPOSITE ||
			functypclass == TYPEFUNC_COMPOSITE_DOMAIN)
		{
			/* Composite data type, e.g. a table's row type */
			Assert(tupdesc);
			/* Must copy it out of typcache for safety */
			sexpr->funcResultDesc = CreateTupleDescCopy(tupdesc);
			sexpr->funcReturnsTuple = true;
		}
		else if (functypclass == TYPEFUNC_SCALAR)
		{
			/* Base data type, i.e. scalar */
			tupdesc = CreateTemplateTupleDesc(1);
			TupleDescInitEntry(tupdesc,
							   (AttrNumber) 1,
							   NULL,
							   funcrettype,
							   -1,
							   0);
			sexpr->funcResultDesc = tupdesc;
			sexpr->funcReturnsTuple = false;
		}
		else if (functypclass == TYPEFUNC_RECORD)
		{
			/* This will work if function doesn't need an expectedDesc */
			sexpr->funcResultDesc = NULL;
			sexpr->funcReturnsTuple = true;
		}
		else
		{
			/* Else, we will fail if function needs an expectedDesc */
			sexpr->funcResultDesc = NULL;
		}

		MemoryContextSwitchTo(oldcontext);
	}
	else
		sexpr->funcResultDesc = NULL;

	/* Initialize additional state */
	sexpr->funcResultStore = NULL;
	sexpr->funcResultSlot = NULL;
	sexpr->shutdown_reg = false;
}

/*
 * callback function in case a SetExprState needs to be shut down before it
 * has been run to completion
 */
static void
ShutdownSetExpr(Datum arg)
{
	SetExprState *sexpr = castNode(SetExprState, DatumGetPointer(arg));

	/* If we have a slot, make sure it's let go of any tuplestore pointer */
	if (sexpr->funcResultSlot)
		ExecClearTuple(sexpr->funcResultSlot);

	/* Release any open tuplestore */
	if (sexpr->funcResultStore)
		tuplestore_end(sexpr->funcResultStore);
	sexpr->funcResultStore = NULL;

	/* Clear any active set-argument state */
	sexpr->setArgsValid = false;

	/* execUtils will deregister the callback... */
	sexpr->shutdown_reg = false;
}

/*
 * Evaluate arguments for a function.
 */
static void
ExecEvalFuncArgs(FunctionCallInfo fcinfo,
				 List *argList,
				 ExprContext *econtext)
{
	int			i;
	ListCell   *arg;

	i = 0;
	foreach(arg, argList)
	{
		ExprState  *argstate = (ExprState *) lfirst(arg);

		fcinfo->args[i].value = ExecEvalExpr(argstate,
											 econtext,
											 &fcinfo->args[i].isnull);
		i++;
	}

	Assert(i == fcinfo->nargs);
}

/*
 *		ExecPrepareTuplestoreResult
 *
 * Subroutine for ExecMakeFunctionResultSet: prepare to extract rows from a
 * tuplestore function result.  We must set up a funcResultSlot (unless
 * already done in a previous call cycle) and verify that the function
 * returned the expected tuple descriptor.
 */
static void
ExecPrepareTuplestoreResult(SetExprState *sexpr,
							ExprContext *econtext,
							Tuplestorestate *resultStore,
							TupleDesc resultDesc)
{
	sexpr->funcResultStore = resultStore;

	if (sexpr->funcResultSlot == NULL)
	{
		/* Create a slot so we can read data out of the tuplestore */
		TupleDesc	slotDesc;
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(sexpr->func.fn_mcxt);

		/*
		 * If we were not able to determine the result rowtype from context,
		 * and the function didn't return a tupdesc, we have to fail.
		 */
		if (sexpr->funcResultDesc)
			slotDesc = sexpr->funcResultDesc;
		else if (resultDesc)
		{
			/* don't assume resultDesc is long-lived */
			slotDesc = CreateTupleDescCopy(resultDesc);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning setof record called in "
							"context that cannot accept type record")));
			slotDesc = NULL;	/* keep compiler quiet */
		}

		sexpr->funcResultSlot = MakeSingleTupleTableSlot(slotDesc,
														 &TTSOpsMinimalTuple);
		MemoryContextSwitchTo(oldcontext);
	}

	/*
	 * If function provided a tupdesc, cross-check it.  We only really need to
	 * do this for functions returning RECORD, but might as well do it always.
	 */
	if (resultDesc)
	{
		if (sexpr->funcResultDesc)
			tupledesc_match(sexpr->funcResultDesc, resultDesc);

		/*
		 * If it is a dynamically-allocated TupleDesc, free it: it is
		 * typically allocated in a per-query context, so we must avoid
		 * leaking it across multiple usages.
		 */
		if (resultDesc->tdrefcount == -1)
			FreeTupleDesc(resultDesc);
	}

	/* Register cleanup callback if we didn't already */
	if (!sexpr->shutdown_reg)
	{
		RegisterExprContextCallback(econtext,
									ShutdownSetExpr,
									PointerGetDatum(sexpr));
		sexpr->shutdown_reg = true;
	}
}

/*
 * Check that function result tuple type (src_tupdesc) matches or can
 * be considered to match what the query expects (dst_tupdesc). If
 * they don't match, ereport.
 *
 * We really only care about number of attributes and data type.
 * Also, we can ignore type mismatch on columns that are dropped in the
 * destination type, so long as the physical storage matches.  This is
 * helpful in some cases involving out-of-date cached plans.
 */
static void
tupledesc_match(TupleDesc dst_tupdesc, TupleDesc src_tupdesc)
{
	int			i;

	if (dst_tupdesc->natts != src_tupdesc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("function return row and query-specified return row do not match"),
				 errdetail_plural("Returned row contains %d attribute, but query expects %d.",
								  "Returned row contains %d attributes, but query expects %d.",
								  src_tupdesc->natts,
								  src_tupdesc->natts, dst_tupdesc->natts)));

	for (i = 0; i < dst_tupdesc->natts; i++)
	{
		Form_pg_attribute dattr = TupleDescAttr(dst_tupdesc, i);
		Form_pg_attribute sattr = TupleDescAttr(src_tupdesc, i);

		if (IsBinaryCoercible(sattr->atttypid, dattr->atttypid))
			continue;			/* no worries */
		if (!dattr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("function return row and query-specified return row do not match"),
					 errdetail("Returned type %s at ordinal position %d, but query expects %s.",
							   format_type_be(sattr->atttypid),
							   i + 1,
							   format_type_be(dattr->atttypid))));

		if (dattr->attlen != sattr->attlen ||
			dattr->attalign != sattr->attalign)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("function return row and query-specified return row do not match"),
					 errdetail("Physical storage mismatch on dropped attribute at ordinal position %d.",
							   i + 1)));
	}
}
