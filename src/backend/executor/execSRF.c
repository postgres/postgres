/*-------------------------------------------------------------------------
 *
 * execSRF.c
 *	  Routines implementing the API for set-returning functions
 *
 * This file serves nodeFunctionscan.c and nodeProjectSet.c, providing
 * common code for calling set-returning functions according to the
 * ReturnSetInfo API.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
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
void
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
