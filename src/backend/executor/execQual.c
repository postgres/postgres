/*-------------------------------------------------------------------------
 *
 * execQual.c
 *	  Routines to evaluate qualification and targetlist expressions
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execQual.c,v 1.150.2.1 2003/12/18 22:23:54 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		ExecEvalExpr	- evaluate an expression and return a datum
 *		ExecEvalExprSwitchContext - same, but switch into eval memory context
 *		ExecQual		- return true/false if qualification is satisfied
 *		ExecProject		- form a new tuple by projecting the given tuple
 *
 *	 NOTES
 *		ExecEvalExpr() and ExecEvalVar() are hotspots.	making these faster
 *		will speed up the entire system.  Unfortunately they are currently
 *		implemented recursively.  Eliminating the recursion is bound to
 *		improve the speed of the executor.
 *
 *		ExecProject() is used to make tuple projections.  Rather then
 *		trying to speed it up, the execution plan should be pre-processed
 *		to facilitate attribute sharing between nodes wherever possible,
 *		instead of doing needless copying.	-cim 5/31/91
 *
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_type.h"
#include "commands/typecmds.h"
#include "executor/execdebug.h"
#include "executor/functions.h"
#include "executor/nodeSubplan.h"
#include "miscadmin.h"
#include "optimizer/planmain.h"
#include "parser/parse_expr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


/* static function decls */
static Datum ExecEvalAggref(AggrefExprState *aggref,
			   ExprContext *econtext,
			   bool *isNull);
static Datum ExecEvalArrayRef(ArrayRefExprState *astate,
				 ExprContext *econtext,
				 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalVar(Var *variable, ExprContext *econtext, bool *isNull);
static Datum ExecEvalParam(Param *expression, ExprContext *econtext,
			  bool *isNull);
static Datum ExecEvalFunc(FuncExprState *fcache, ExprContext *econtext,
			 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalOper(FuncExprState *fcache, ExprContext *econtext,
			 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalDistinct(FuncExprState *fcache, ExprContext *econtext,
				 bool *isNull);
static Datum ExecEvalScalarArrayOp(ScalarArrayOpExprState *sstate,
					  ExprContext *econtext, bool *isNull);
static ExprDoneCond ExecEvalFuncArgs(FunctionCallInfo fcinfo,
				 List *argList, ExprContext *econtext);
static Datum ExecEvalNot(BoolExprState *notclause, ExprContext *econtext,
			bool *isNull);
static Datum ExecEvalOr(BoolExprState *orExpr, ExprContext *econtext,
		   bool *isNull);
static Datum ExecEvalAnd(BoolExprState *andExpr, ExprContext *econtext,
			bool *isNull);
static Datum ExecEvalCase(CaseExprState *caseExpr, ExprContext *econtext,
			 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalArray(ArrayExprState *astate,
			  ExprContext *econtext,
			  bool *isNull);
static Datum ExecEvalCoalesce(CoalesceExprState *coalesceExpr,
				 ExprContext *econtext,
				 bool *isNull);
static Datum ExecEvalNullIf(FuncExprState *nullIfExpr, ExprContext *econtext,
			   bool *isNull);
static Datum ExecEvalNullTest(GenericExprState *nstate,
				 ExprContext *econtext,
				 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalBooleanTest(GenericExprState *bstate,
					ExprContext *econtext,
					bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalCoerceToDomain(CoerceToDomainState *cstate,
					   ExprContext *econtext,
					   bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalCoerceToDomainValue(CoerceToDomainValue *conVal,
							ExprContext *econtext, bool *isNull);
static Datum ExecEvalFieldSelect(GenericExprState *fstate,
					ExprContext *econtext,
					bool *isNull, ExprDoneCond *isDone);


/*----------
 *	  ExecEvalArrayRef
 *
 *	   This function takes an ArrayRef and returns the extracted Datum
 *	   if it's a simple reference, or the modified array value if it's
 *	   an array assignment (i.e., array element or slice insertion).
 *
 * NOTE: if we get a NULL result from a subexpression, we return NULL when
 * it's an array reference, or the unmodified source array when it's an
 * array assignment.  This may seem peculiar, but if we return NULL (as was
 * done in versions up through 7.0) then an assignment like
 *			UPDATE table SET arrayfield[4] = NULL
 * will result in setting the whole array to NULL, which is certainly not
 * very desirable.	By returning the source array we make the assignment
 * into a no-op, instead.  (Eventually we need to redesign arrays so that
 * individual elements can be NULL, but for now, let's try to protect users
 * from shooting themselves in the foot.)
 *
 * NOTE: we deliberately refrain from applying DatumGetArrayTypeP() here,
 * even though that might seem natural, because this code needs to support
 * both varlena arrays and fixed-length array types.  DatumGetArrayTypeP()
 * only works for the varlena kind.  The routines we call in arrayfuncs.c
 * have to know the difference (that's what they need refattrlength for).
 *----------
 */
static Datum
ExecEvalArrayRef(ArrayRefExprState *astate,
				 ExprContext *econtext,
				 bool *isNull,
				 ExprDoneCond *isDone)
{
	ArrayRef   *arrayRef = (ArrayRef *) astate->xprstate.expr;
	ArrayType  *array_source;
	ArrayType  *resultArray;
	bool		isAssignment = (arrayRef->refassgnexpr != NULL);
	List	   *elt;
	int			i = 0,
				j = 0;
	IntArray	upper,
				lower;
	int		   *lIndex;

	if (arrayRef->refexpr != NULL)
	{
		array_source = (ArrayType *)
			DatumGetPointer(ExecEvalExpr(astate->refexpr,
										 econtext,
										 isNull,
										 isDone));

		/*
		 * If refexpr yields NULL, result is always NULL, for now anyway.
		 * (This means you cannot assign to an element or slice of an
		 * array that's NULL; it'll just stay NULL.)
		 */
		if (*isNull)
			return (Datum) NULL;
	}
	else
	{
		/*
		 * Empty refexpr indicates we are doing an INSERT into an array
		 * column. For now, we just take the refassgnexpr (which the
		 * parser will have ensured is an array value) and return it
		 * as-is, ignoring any subscripts that may have been supplied in
		 * the INSERT column list. This is a kluge, but it's not real
		 * clear what the semantics ought to be...
		 */
		array_source = NULL;
	}

	foreach(elt, astate->refupperindexpr)
	{
		if (i >= MAXDIM)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
							i, MAXDIM)));

		upper.indx[i++] = DatumGetInt32(ExecEvalExpr((ExprState *) lfirst(elt),
													 econtext,
													 isNull,
													 NULL));
		/* If any index expr yields NULL, result is NULL or source array */
		if (*isNull)
		{
			if (!isAssignment || array_source == NULL)
				return (Datum) NULL;
			*isNull = false;
			return PointerGetDatum(array_source);
		}
	}

	if (astate->reflowerindexpr != NIL)
	{
		foreach(elt, astate->reflowerindexpr)
		{
			if (j >= MAXDIM)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
								i, MAXDIM)));

			lower.indx[j++] = DatumGetInt32(ExecEvalExpr((ExprState *) lfirst(elt),
														 econtext,
														 isNull,
														 NULL));

			/*
			 * If any index expr yields NULL, result is NULL or source
			 * array
			 */
			if (*isNull)
			{
				if (!isAssignment || array_source == NULL)
					return (Datum) NULL;
				*isNull = false;
				return PointerGetDatum(array_source);
			}
		}
		/* this can't happen unless parser messed up */
		if (i != j)
			elog(ERROR, "upper and lower index lists are not same length");
		lIndex = lower.indx;
	}
	else
		lIndex = NULL;

	if (isAssignment)
	{
		Datum		sourceData = ExecEvalExpr(astate->refassgnexpr,
											  econtext,
											  isNull,
											  NULL);

		/*
		 * For now, can't cope with inserting NULL into an array, so make
		 * it a no-op per discussion above...
		 */
		if (*isNull)
		{
			if (array_source == NULL)
				return (Datum) NULL;
			*isNull = false;
			return PointerGetDatum(array_source);
		}

		if (array_source == NULL)
			return sourceData;	/* XXX do something else? */

		if (lIndex == NULL)
			resultArray = array_set(array_source, i,
									upper.indx,
									sourceData,
									astate->refattrlength,
									astate->refelemlength,
									astate->refelembyval,
									astate->refelemalign,
									isNull);
		else
			resultArray = array_set_slice(array_source, i,
										  upper.indx, lower.indx,
							   (ArrayType *) DatumGetPointer(sourceData),
										  astate->refattrlength,
										  astate->refelemlength,
										  astate->refelembyval,
										  astate->refelemalign,
										  isNull);
		return PointerGetDatum(resultArray);
	}

	if (lIndex == NULL)
		return array_ref(array_source, i, upper.indx,
						 astate->refattrlength,
						 astate->refelemlength,
						 astate->refelembyval,
						 astate->refelemalign,
						 isNull);
	else
	{
		resultArray = array_get_slice(array_source, i,
									  upper.indx, lower.indx,
									  astate->refattrlength,
									  astate->refelemlength,
									  astate->refelembyval,
									  astate->refelemalign,
									  isNull);
		return PointerGetDatum(resultArray);
	}
}


/* ----------------------------------------------------------------
 *		ExecEvalAggref
 *
 *		Returns a Datum whose value is the value of the precomputed
 *		aggregate found in the given expression context.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalAggref(AggrefExprState *aggref, ExprContext *econtext, bool *isNull)
{
	if (econtext->ecxt_aggvalues == NULL)		/* safety check */
		elog(ERROR, "no aggregates in this expression context");

	*isNull = econtext->ecxt_aggnulls[aggref->aggno];
	return econtext->ecxt_aggvalues[aggref->aggno];
}

/* ----------------------------------------------------------------
 *		ExecEvalVar
 *
 *		Returns a Datum whose value is the value of a range
 *		variable with respect to given expression context.
 * ---------------------------------------------------------------- */
static Datum
ExecEvalVar(Var *variable, ExprContext *econtext, bool *isNull)
{
	Datum		result;
	TupleTableSlot *slot;
	AttrNumber	attnum;
	HeapTuple	heapTuple;
	TupleDesc	tuple_type;

	/*
	 * get the slot we want
	 */
	switch (variable->varno)
	{
		case INNER:				/* get the tuple from the inner node */
			slot = econtext->ecxt_innertuple;
			break;

		case OUTER:				/* get the tuple from the outer node */
			slot = econtext->ecxt_outertuple;
			break;

		default:				/* get the tuple from the relation being
								 * scanned */
			slot = econtext->ecxt_scantuple;
			break;
	}

	/*
	 * extract tuple information from the slot
	 */
	heapTuple = slot->val;
	tuple_type = slot->ttc_tupleDescriptor;

	attnum = variable->varattno;

	/*
	 * Some checks that are only applied for user attribute numbers
	 * (bogus system attnums will be caught inside heap_getattr).
	 */
	if (attnum > 0)
	{
		/*
		 * This assert checks that the attnum is valid.
		 */
		Assert(attnum <= tuple_type->natts &&
			   tuple_type->attrs[attnum - 1] != NULL);

		/*
		 * If the attribute's column has been dropped, we force a NULL result.
		 * This case should not happen in normal use, but it could happen if
		 * we are executing a plan cached before the column was dropped.
		 */
		if (tuple_type->attrs[attnum - 1]->attisdropped)
		{
			*isNull = true;
			return (Datum) 0;
		}

		/*
		 * This assert checks that the datatype the plan expects to get (as
		 * told by our "variable" argument) is in fact the datatype of the
		 * attribute being fetched (as seen in the current context, identified
		 * by our "econtext" argument).  Otherwise crashes are likely.
		 *
		 * Note that we can't check dropped columns, since their atttypid
		 * has been zeroed.
		 */
		Assert(variable->vartype == tuple_type->attrs[attnum - 1]->atttypid);
	}

	/*
	 * If the attribute number is invalid, then we are supposed to return
	 * the entire tuple; we give back a whole slot so that callers know
	 * what the tuple looks like.
	 *
	 * XXX this is a horrid crock: since the pointer to the slot might live
	 * longer than the current evaluation context, we are forced to copy
	 * the tuple and slot into a long-lived context --- we use the
	 * econtext's per-query memory which should be safe enough.  This
	 * represents a serious memory leak if many such tuples are processed
	 * in one command, however.  We ought to redesign the representation
	 * of whole-tuple datums so that this is not necessary.
	 *
	 * We assume it's OK to point to the existing tupleDescriptor, rather
	 * than copy that too.
	 */
	if (attnum == InvalidAttrNumber)
	{
		MemoryContext oldContext;
		TupleTableSlot *tempSlot;
		HeapTuple	tup;

		oldContext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
		tempSlot = MakeTupleTableSlot();
		tup = heap_copytuple(heapTuple);
		ExecStoreTuple(tup, tempSlot, InvalidBuffer, true);
		ExecSetSlotDescriptor(tempSlot, tuple_type, false);
		MemoryContextSwitchTo(oldContext);
		return PointerGetDatum(tempSlot);
	}

	result = heap_getattr(heapTuple,	/* tuple containing attribute */
						  attnum,		/* attribute number of desired
										 * attribute */
						  tuple_type,	/* tuple descriptor of tuple */
						  isNull);		/* return: is attribute null? */

	return result;
}

/* ----------------------------------------------------------------
 *		ExecEvalParam
 *
 *		Returns the value of a parameter.  A param node contains
 *		something like ($.name) and the expression context contains
 *		the current parameter bindings (name = "sam") (age = 34)...
 *		so our job is to find and return the appropriate datum ("sam").
 *
 *		Q: if we have a parameter ($.foo) without a binding, i.e.
 *		   there is no (foo = xxx) in the parameter list info,
 *		   is this a fatal error or should this be a "not available"
 *		   (in which case we could return NULL)?	-cim 10/13/89
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalParam(Param *expression, ExprContext *econtext, bool *isNull)
{
	int			thisParamKind = expression->paramkind;
	AttrNumber	thisParamId = expression->paramid;

	if (thisParamKind == PARAM_EXEC)
	{
		/*
		 * PARAM_EXEC params (internal executor parameters) are stored in
		 * the ecxt_param_exec_vals array, and can be accessed by array
		 * index.
		 */
		ParamExecData *prm;

		prm = &(econtext->ecxt_param_exec_vals[thisParamId]);
		if (prm->execPlan != NULL)
		{
			/* Parameter not evaluated yet, so go do it */
			ExecSetParamPlan(prm->execPlan, econtext);
			/* ExecSetParamPlan should have processed this param... */
			Assert(prm->execPlan == NULL);
		}
		*isNull = prm->isnull;
		return prm->value;
	}
	else
	{
		/*
		 * All other parameter types must be sought in
		 * ecxt_param_list_info. NOTE: The last entry in the param array
		 * is always an entry with kind == PARAM_INVALID.
		 */
		ParamListInfo paramList = econtext->ecxt_param_list_info;
		char	   *thisParamName = expression->paramname;
		bool		matchFound = false;

		if (paramList != NULL)
		{
			while (paramList->kind != PARAM_INVALID && !matchFound)
			{
				if (thisParamKind == paramList->kind)
				{
					switch (thisParamKind)
					{
						case PARAM_NAMED:
							if (strcmp(paramList->name, thisParamName) == 0)
								matchFound = true;
							break;
						case PARAM_NUM:
							if (paramList->id == thisParamId)
								matchFound = true;
							break;
						default:
							elog(ERROR, "unrecognized paramkind: %d",
								 thisParamKind);
					}
				}
				if (!matchFound)
					paramList++;
			}					/* while */
		}						/* if */

		if (!matchFound)
		{
			if (thisParamKind == PARAM_NAMED)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("no value found for parameter \"%s\"",
								thisParamName)));
			else
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("no value found for parameter %d",
								thisParamId)));
		}

		*isNull = paramList->isnull;
		return paramList->value;
	}
}


/* ----------------------------------------------------------------
 *		ExecEvalOper / ExecEvalFunc support routines
 * ----------------------------------------------------------------
 */

/*
 *		GetAttributeByName
 *		GetAttributeByNum
 *
 *		These are functions which return the value of the
 *		named attribute out of the tuple from the arg slot.  User defined
 *		C functions which take a tuple as an argument are expected
 *		to use this.  Ex: overpaid(EMP) might call GetAttributeByNum().
 */
Datum
GetAttributeByNum(TupleTableSlot *slot,
				  AttrNumber attrno,
				  bool *isNull)
{
	Datum		retval;

	if (!AttributeNumberIsValid(attrno))
		elog(ERROR, "invalid attribute number %d", attrno);

	if (isNull == (bool *) NULL)
		elog(ERROR, "a NULL isNull pointer was passed");

	if (TupIsNull(slot))
	{
		*isNull = true;
		return (Datum) 0;
	}

	retval = heap_getattr(slot->val,
						  attrno,
						  slot->ttc_tupleDescriptor,
						  isNull);
	if (*isNull)
		return (Datum) 0;

	return retval;
}

Datum
GetAttributeByName(TupleTableSlot *slot, char *attname, bool *isNull)
{
	AttrNumber	attrno;
	TupleDesc	tupdesc;
	Datum		retval;
	int			natts;
	int			i;

	if (attname == NULL)
		elog(ERROR, "invalid attribute name");

	if (isNull == (bool *) NULL)
		elog(ERROR, "a NULL isNull pointer was passed");

	if (TupIsNull(slot))
	{
		*isNull = true;
		return (Datum) 0;
	}

	tupdesc = slot->ttc_tupleDescriptor;
	natts = slot->val->t_data->t_natts;

	attrno = InvalidAttrNumber;
	for (i = 0; i < tupdesc->natts; i++)
	{
		if (namestrcmp(&(tupdesc->attrs[i]->attname), attname) == 0)
		{
			attrno = tupdesc->attrs[i]->attnum;
			break;
		}
	}

	if (attrno == InvalidAttrNumber)
		elog(ERROR, "attribute \"%s\" does not exist", attname);

	retval = heap_getattr(slot->val,
						  attrno,
						  tupdesc,
						  isNull);
	if (*isNull)
		return (Datum) 0;

	return retval;
}

/*
 * init_fcache - initialize a FuncExprState node during first use
 */
void
init_fcache(Oid foid, FuncExprState *fcache, MemoryContext fcacheCxt)
{
	AclResult	aclresult;

	/* Check permission to call function */
	aclresult = pg_proc_aclcheck(foid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_PROC, get_func_name(foid));

	/* Safety check (should never fail, as parser should check sooner) */
	if (length(fcache->args) > FUNC_MAX_ARGS)
		elog(ERROR, "too many arguments");

	/* Set up the primary fmgr lookup information */
	fmgr_info_cxt(foid, &(fcache->func), fcacheCxt);

	/* Initialize additional info */
	fcache->setArgsValid = false;
	fcache->shutdown_reg = false;
	fcache->func.fn_expr = (Node *) fcache->xprstate.expr;
}

/*
 * callback function in case a FuncExpr returning a set needs to be shut down
 * before it has been run to completion
 */
static void
ShutdownFuncExpr(Datum arg)
{
	FuncExprState *fcache = (FuncExprState *) DatumGetPointer(arg);

	/* Clear any active set-argument state */
	fcache->setArgsValid = false;

	/* execUtils will deregister the callback... */
	fcache->shutdown_reg = false;
}

/*
 * Evaluate arguments for a function.
 */
static ExprDoneCond
ExecEvalFuncArgs(FunctionCallInfo fcinfo,
				 List *argList,
				 ExprContext *econtext)
{
	ExprDoneCond argIsDone;
	int			i;
	List	   *arg;

	argIsDone = ExprSingleResult;		/* default assumption */

	i = 0;
	foreach(arg, argList)
	{
		ExprDoneCond thisArgIsDone;

		fcinfo->arg[i] = ExecEvalExpr((ExprState *) lfirst(arg),
									  econtext,
									  &fcinfo->argnull[i],
									  &thisArgIsDone);

		if (thisArgIsDone != ExprSingleResult)
		{
			/*
			 * We allow only one argument to have a set value; we'd need
			 * much more complexity to keep track of multiple set
			 * arguments (cf. ExecTargetList) and it doesn't seem worth
			 * it.
			 */
			if (argIsDone != ExprSingleResult)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("functions and operators can take at most one set argument")));
			argIsDone = thisArgIsDone;
		}
		i++;
	}

	fcinfo->nargs = i;

	return argIsDone;
}

/*
 *		ExecMakeFunctionResult
 *
 * Evaluate the arguments to a function and then the function itself.
 */
Datum
ExecMakeFunctionResult(FuncExprState *fcache,
					   ExprContext *econtext,
					   bool *isNull,
					   ExprDoneCond *isDone)
{
	List	   *arguments = fcache->args;
	Datum		result;
	FunctionCallInfoData fcinfo;
	ReturnSetInfo rsinfo;		/* for functions returning sets */
	ExprDoneCond argDone;
	bool		hasSetArg;
	int			i;

	/*
	 * arguments is a list of expressions to evaluate before passing to
	 * the function manager.  We skip the evaluation if it was already
	 * done in the previous call (ie, we are continuing the evaluation of
	 * a set-valued function).	Otherwise, collect the current argument
	 * values into fcinfo.
	 */
	if (!fcache->setArgsValid)
	{
		/* Need to prep callinfo structure */
		MemSet(&fcinfo, 0, sizeof(fcinfo));
		fcinfo.flinfo = &(fcache->func);
		argDone = ExecEvalFuncArgs(&fcinfo, arguments, econtext);
		if (argDone == ExprEndResult)
		{
			/* input is an empty set, so return an empty set. */
			*isNull = true;
			if (isDone)
				*isDone = ExprEndResult;
			else
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("set-valued function called in context that cannot accept a set")));
			return (Datum) 0;
		}
		hasSetArg = (argDone != ExprSingleResult);
	}
	else
	{
		/* Copy callinfo from previous evaluation */
		memcpy(&fcinfo, &fcache->setArgs, sizeof(fcinfo));
		hasSetArg = fcache->setHasSetArg;
		/* Reset flag (we may set it again below) */
		fcache->setArgsValid = false;
	}

	/*
	 * If function returns set, prepare a resultinfo node for
	 * communication
	 */
	if (fcache->func.fn_retset)
	{
		fcinfo.resultinfo = (Node *) &rsinfo;
		rsinfo.type = T_ReturnSetInfo;
		rsinfo.econtext = econtext;
		rsinfo.expectedDesc = NULL;
		rsinfo.allowedModes = (int) SFRM_ValuePerCall;
		rsinfo.returnMode = SFRM_ValuePerCall;
		/* isDone is filled below */
		rsinfo.setResult = NULL;
		rsinfo.setDesc = NULL;
	}

	/*
	 * now return the value gotten by calling the function manager,
	 * passing the function the evaluated parameter values.
	 */
	if (fcache->func.fn_retset || hasSetArg)
	{
		/*
		 * We need to return a set result.	Complain if caller not ready
		 * to accept one.
		 */
		if (isDone == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("set-valued function called in context that cannot accept a set")));

		/*
		 * This loop handles the situation where we have both a set
		 * argument and a set-valued function.	Once we have exhausted the
		 * function's value(s) for a particular argument value, we have to
		 * get the next argument value and start the function over again.
		 * We might have to do it more than once, if the function produces
		 * an empty result set for a particular input value.
		 */
		for (;;)
		{
			/*
			 * If function is strict, and there are any NULL arguments,
			 * skip calling the function (at least for this set of args).
			 */
			bool		callit = true;

			if (fcache->func.fn_strict)
			{
				for (i = 0; i < fcinfo.nargs; i++)
				{
					if (fcinfo.argnull[i])
					{
						callit = false;
						break;
					}
				}
			}

			if (callit)
			{
				fcinfo.isnull = false;
				rsinfo.isDone = ExprSingleResult;
				result = FunctionCallInvoke(&fcinfo);
				*isNull = fcinfo.isnull;
				*isDone = rsinfo.isDone;
			}
			else
			{
				result = (Datum) 0;
				*isNull = true;
				*isDone = ExprEndResult;
			}

			if (*isDone != ExprEndResult)
			{
				/*
				 * Got a result from current argument.	If function itself
				 * returns set, save the current argument values to re-use
				 * on the next call.
				 */
				if (fcache->func.fn_retset)
				{
					memcpy(&fcache->setArgs, &fcinfo, sizeof(fcinfo));
					fcache->setHasSetArg = hasSetArg;
					fcache->setArgsValid = true;
					/* Register cleanup callback if we didn't already */
					if (!fcache->shutdown_reg)
					{
						RegisterExprContextCallback(econtext,
													ShutdownFuncExpr,
													PointerGetDatum(fcache));
						fcache->shutdown_reg = true;
					}
				}

				/*
				 * Make sure we say we are returning a set, even if the
				 * function itself doesn't return sets.
				 */
				*isDone = ExprMultipleResult;
				break;
			}

			/* Else, done with this argument */
			if (!hasSetArg)
				break;			/* input not a set, so done */

			/* Re-eval args to get the next element of the input set */
			argDone = ExecEvalFuncArgs(&fcinfo, arguments, econtext);

			if (argDone != ExprMultipleResult)
			{
				/* End of argument set, so we're done. */
				*isNull = true;
				*isDone = ExprEndResult;
				result = (Datum) 0;
				break;
			}

			/*
			 * If we reach here, loop around to run the function on the
			 * new argument.
			 */
		}
	}
	else
	{
		/*
		 * Non-set case: much easier.
		 *
		 * If function is strict, and there are any NULL arguments, skip
		 * calling the function and return NULL.
		 */
		if (fcache->func.fn_strict)
		{
			for (i = 0; i < fcinfo.nargs; i++)
			{
				if (fcinfo.argnull[i])
				{
					*isNull = true;
					return (Datum) 0;
				}
			}
		}
		fcinfo.isnull = false;
		result = FunctionCallInvoke(&fcinfo);
		*isNull = fcinfo.isnull;
	}

	return result;
}


/*
 *		ExecMakeTableFunctionResult
 *
 * Evaluate a table function, producing a materialized result in a Tuplestore
 * object.	(If function returns an empty set, we just return NULL instead.)
 */
Tuplestorestate *
ExecMakeTableFunctionResult(ExprState *funcexpr,
							ExprContext *econtext,
							TupleDesc expectedDesc,
							TupleDesc *returnDesc)
{
	Tuplestorestate *tupstore = NULL;
	TupleDesc	tupdesc = NULL;
	Oid			funcrettype;
	FunctionCallInfoData fcinfo;
	ReturnSetInfo rsinfo;
	MemoryContext callerContext;
	MemoryContext oldcontext;
	TupleTableSlot *slot;
	bool		direct_function_call;
	bool		first_time = true;
	bool		returnsTuple = false;

	/*
	 * Normally the passed expression tree will be a FuncExprState, since
	 * the grammar only allows a function call at the top level of a table
	 * function reference.	However, if the function doesn't return set
	 * then the planner might have replaced the function call via
	 * constant-folding or inlining.  So if we see any other kind of
	 * expression node, execute it via the general ExecEvalExpr() code;
	 * the only difference is that we don't get a chance to pass a special
	 * ReturnSetInfo to any functions buried in the expression.
	 */
	if (funcexpr && IsA(funcexpr, FuncExprState) &&
		IsA(funcexpr->expr, FuncExpr))
	{
		FuncExprState *fcache = (FuncExprState *) funcexpr;
		ExprDoneCond argDone;

		/*
		 * This path is similar to ExecMakeFunctionResult.
		 */
		direct_function_call = true;

		/*
		 * Initialize function cache if first time through
		 */
		if (fcache->func.fn_oid == InvalidOid)
		{
			FuncExpr   *func = (FuncExpr *) fcache->xprstate.expr;

			init_fcache(func->funcid, fcache, econtext->ecxt_per_query_memory);
		}

		/*
		 * Evaluate the function's argument list.
		 *
		 * Note: ideally, we'd do this in the per-tuple context, but then the
		 * argument values would disappear when we reset the context in
		 * the inner loop.	So do it in caller context.  Perhaps we should
		 * make a separate context just to hold the evaluated arguments?
		 */
		MemSet(&fcinfo, 0, sizeof(fcinfo));
		fcinfo.flinfo = &(fcache->func);
		argDone = ExecEvalFuncArgs(&fcinfo, fcache->args, econtext);
		/* We don't allow sets in the arguments of the table function */
		if (argDone != ExprSingleResult)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("set-valued function called in context that cannot accept a set")));

		/*
		 * If function is strict, and there are any NULL arguments, skip
		 * calling the function and return NULL (actually an empty set).
		 */
		if (fcache->func.fn_strict)
		{
			int			i;

			for (i = 0; i < fcinfo.nargs; i++)
			{
				if (fcinfo.argnull[i])
				{
					*returnDesc = NULL;
					return NULL;
				}
			}
		}
	}
	else
	{
		/* Treat funcexpr as a generic expression */
		direct_function_call = false;
	}

	funcrettype = exprType((Node *) funcexpr->expr);

	/*
	 * Prepare a resultinfo node for communication.  We always do this
	 * even if not expecting a set result, so that we can pass
	 * expectedDesc.  In the generic-expression case, the expression
	 * doesn't actually get to see the resultinfo, but set it up anyway
	 * because we use some of the fields as our own state variables.
	 */
	fcinfo.resultinfo = (Node *) &rsinfo;
	rsinfo.type = T_ReturnSetInfo;
	rsinfo.econtext = econtext;
	rsinfo.expectedDesc = expectedDesc;
	rsinfo.allowedModes = (int) (SFRM_ValuePerCall | SFRM_Materialize);
	rsinfo.returnMode = SFRM_ValuePerCall;
	/* isDone is filled below */
	rsinfo.setResult = NULL;
	rsinfo.setDesc = NULL;

	/*
	 * Switch to short-lived context for calling the function or
	 * expression.
	 */
	callerContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/*
	 * Loop to handle the ValuePerCall protocol (which is also the same
	 * behavior needed in the generic ExecEvalExpr path).
	 */
	for (;;)
	{
		Datum		result;
		HeapTuple	tuple;

		/*
		 * reset per-tuple memory context before each call of the function
		 * or expression. This cleans up any local memory the function may
		 * leak when called.
		 */
		ResetExprContext(econtext);

		/* Call the function or expression one time */
		if (direct_function_call)
		{
			fcinfo.isnull = false;
			rsinfo.isDone = ExprSingleResult;
			result = FunctionCallInvoke(&fcinfo);
		}
		else
		{
			result = ExecEvalExpr(funcexpr, econtext,
								  &fcinfo.isnull, &rsinfo.isDone);
		}

		/* Which protocol does function want to use? */
		if (rsinfo.returnMode == SFRM_ValuePerCall)
		{
			/*
			 * Check for end of result set.
			 *
			 * Note: if function returns an empty set, we don't build a
			 * tupdesc or tuplestore (since we can't get a tupdesc in the
			 * function-returning-tuple case)
			 */
			if (rsinfo.isDone == ExprEndResult)
				break;

			/*
			 * If first time through, build tupdesc and tuplestore for
			 * result
			 */
			if (first_time)
			{
				oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
				if (funcrettype == RECORDOID ||
					get_typtype(funcrettype) == 'c')
				{
					/*
					 * Composite type, so function should have returned a
					 * TupleTableSlot; use its descriptor
					 */
					slot = (TupleTableSlot *) DatumGetPointer(result);
					if (fcinfo.isnull || !slot)
						ereport(ERROR,
								(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
								 errmsg("function returning row cannot return null value")));
					if (!IsA(slot, TupleTableSlot) ||
						!slot->ttc_tupleDescriptor)
						ereport(ERROR,
								(errcode(ERRCODE_DATATYPE_MISMATCH),
								 errmsg("function returning row did not return a valid tuple slot")));
					tupdesc = CreateTupleDescCopy(slot->ttc_tupleDescriptor);
					returnsTuple = true;
				}
				else
				{
					/*
					 * Scalar type, so make a single-column descriptor
					 */
					tupdesc = CreateTemplateTupleDesc(1, false);
					TupleDescInitEntry(tupdesc,
									   (AttrNumber) 1,
									   "column",
									   funcrettype,
									   -1,
									   0,
									   false);
				}
				tupstore = tuplestore_begin_heap(true, false, SortMem);
				MemoryContextSwitchTo(oldcontext);
				rsinfo.setResult = tupstore;
				rsinfo.setDesc = tupdesc;
			}

			/*
			 * Store current resultset item.
			 */
			if (returnsTuple)
			{
				slot = (TupleTableSlot *) DatumGetPointer(result);
				if (fcinfo.isnull ||
					!slot ||
					!IsA(slot, TupleTableSlot) ||
					TupIsNull(slot))
					ereport(ERROR,
							(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							 errmsg("function returning row cannot return null value")));
				tuple = slot->val;
			}
			else
			{
				char		nullflag;

				nullflag = fcinfo.isnull ? 'n' : ' ';
				tuple = heap_formtuple(tupdesc, &result, &nullflag);
			}

			oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
			tuplestore_puttuple(tupstore, tuple);
			MemoryContextSwitchTo(oldcontext);

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

	MemoryContextSwitchTo(callerContext);

	/* The returned pointers are those in rsinfo */
	*returnDesc = rsinfo.setDesc;
	return rsinfo.setResult;
}


/* ----------------------------------------------------------------
 *		ExecEvalFunc
 *		ExecEvalOper
 *
 *		Evaluate the functional result of a list of arguments by calling the
 *		function manager.
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecEvalFunc
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalFunc(FuncExprState *fcache,
			 ExprContext *econtext,
			 bool *isNull,
			 ExprDoneCond *isDone)
{
	/*
	 * Initialize function cache if first time through
	 */
	if (fcache->func.fn_oid == InvalidOid)
	{
		FuncExpr   *func = (FuncExpr *) fcache->xprstate.expr;

		init_fcache(func->funcid, fcache, econtext->ecxt_per_query_memory);
	}

	return ExecMakeFunctionResult(fcache, econtext, isNull, isDone);
}

/* ----------------------------------------------------------------
 *		ExecEvalOper
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalOper(FuncExprState *fcache,
			 ExprContext *econtext,
			 bool *isNull,
			 ExprDoneCond *isDone)
{
	/*
	 * Initialize function cache if first time through
	 */
	if (fcache->func.fn_oid == InvalidOid)
	{
		OpExpr	   *op = (OpExpr *) fcache->xprstate.expr;

		init_fcache(op->opfuncid, fcache, econtext->ecxt_per_query_memory);
	}

	return ExecMakeFunctionResult(fcache, econtext, isNull, isDone);
}

/* ----------------------------------------------------------------
 *		ExecEvalDistinct
 *
 * IS DISTINCT FROM must evaluate arguments to determine whether
 * they are NULL; if either is NULL then the result is already
 * known. If neither is NULL, then proceed to evaluate the
 * function. Note that this is *always* derived from the equals
 * operator, but since we need special processing of the arguments
 * we can not simply reuse ExecEvalOper() or ExecEvalFunc().
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalDistinct(FuncExprState *fcache,
				 ExprContext *econtext,
				 bool *isNull)
{
	Datum		result;
	FunctionCallInfoData fcinfo;
	ExprDoneCond argDone;
	List	   *argList;

	/*
	 * Initialize function cache if first time through
	 */
	if (fcache->func.fn_oid == InvalidOid)
	{
		DistinctExpr *op = (DistinctExpr *) fcache->xprstate.expr;

		init_fcache(op->opfuncid, fcache, econtext->ecxt_per_query_memory);
		Assert(!fcache->func.fn_retset);
	}

	/*
	 * extract info from fcache
	 */
	argList = fcache->args;

	/* Need to prep callinfo structure */
	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.flinfo = &(fcache->func);
	argDone = ExecEvalFuncArgs(&fcinfo, argList, econtext);
	if (argDone != ExprSingleResult)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("IS DISTINCT FROM does not support set arguments")));
	Assert(fcinfo.nargs == 2);

	if (fcinfo.argnull[0] && fcinfo.argnull[1])
	{
		/* Both NULL? Then is not distinct... */
		result = BoolGetDatum(FALSE);
	}
	else if (fcinfo.argnull[0] || fcinfo.argnull[1])
	{
		/* Only one is NULL? Then is distinct... */
		result = BoolGetDatum(TRUE);
	}
	else
	{
		fcinfo.isnull = false;
		result = FunctionCallInvoke(&fcinfo);
		*isNull = fcinfo.isnull;
		/* Must invert result of "=" */
		result = BoolGetDatum(!DatumGetBool(result));
	}

	return result;
}

/*
 * ExecEvalScalarArrayOp
 *
 * Evaluate "scalar op ANY/ALL (array)".  The operator always yields boolean,
 * and we combine the results across all array elements using OR and AND
 * (for ANY and ALL respectively).	Of course we short-circuit as soon as
 * the result is known.
 */
static Datum
ExecEvalScalarArrayOp(ScalarArrayOpExprState *sstate,
					  ExprContext *econtext, bool *isNull)
{
	ScalarArrayOpExpr *opexpr = (ScalarArrayOpExpr *) sstate->fxprstate.xprstate.expr;
	bool		useOr = opexpr->useOr;
	ArrayType  *arr;
	int			nitems;
	Datum		result;
	bool		resultnull;
	FunctionCallInfoData fcinfo;
	ExprDoneCond argDone;
	int			i;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char	   *s;

	/*
	 * Initialize function cache if first time through
	 */
	if (sstate->fxprstate.func.fn_oid == InvalidOid)
	{
		init_fcache(opexpr->opfuncid, &sstate->fxprstate,
					econtext->ecxt_per_query_memory);
		Assert(!sstate->fxprstate.func.fn_retset);
	}

	/* Need to prep callinfo structure */
	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.flinfo = &(sstate->fxprstate.func);
	argDone = ExecEvalFuncArgs(&fcinfo, sstate->fxprstate.args, econtext);
	if (argDone != ExprSingleResult)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
		   errmsg("op ANY/ALL (array) does not support set arguments")));
	Assert(fcinfo.nargs == 2);

	/*
	 * If the array is NULL then we return NULL --- it's not very
	 * meaningful to do anything else, even if the operator isn't strict.
	 */
	if (fcinfo.argnull[1])
	{
		*isNull = true;
		return (Datum) 0;
	}
	/* Else okay to fetch and detoast the array */
	arr = DatumGetArrayTypeP(fcinfo.arg[1]);

	/*
	 * If the array is empty, we return either FALSE or TRUE per the useOr
	 * flag.  This is correct even if the scalar is NULL; since we would
	 * evaluate the operator zero times, it matters not whether it would
	 * want to return NULL.
	 */
	nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
	if (nitems <= 0)
		return BoolGetDatum(!useOr);

	/*
	 * If the scalar is NULL, and the function is strict, return NULL.
	 * This is just to avoid having to test for strictness inside the
	 * loop.  (XXX but if arrays could have null elements, we'd need a
	 * test anyway.)
	 */
	if (fcinfo.argnull[0] && sstate->fxprstate.func.fn_strict)
	{
		*isNull = true;
		return (Datum) 0;
	}

	/*
	 * We arrange to look up info about the element type only once per
	 * series of calls, assuming the element type doesn't change
	 * underneath us.
	 */
	if (sstate->element_type != ARR_ELEMTYPE(arr))
	{
		get_typlenbyvalalign(ARR_ELEMTYPE(arr),
							 &sstate->typlen,
							 &sstate->typbyval,
							 &sstate->typalign);
		sstate->element_type = ARR_ELEMTYPE(arr);
	}
	typlen = sstate->typlen;
	typbyval = sstate->typbyval;
	typalign = sstate->typalign;

	result = BoolGetDatum(!useOr);
	resultnull = false;

	/* Loop over the array elements */
	s = (char *) ARR_DATA_PTR(arr);
	for (i = 0; i < nitems; i++)
	{
		Datum		elt;
		Datum		thisresult;

		/* Get array element */
		elt = fetch_att(s, typbyval, typlen);

		s = att_addlength(s, typlen, PointerGetDatum(s));
		s = (char *) att_align(s, typalign);

		/* Call comparison function */
		fcinfo.arg[1] = elt;
		fcinfo.argnull[1] = false;
		fcinfo.isnull = false;
		thisresult = FunctionCallInvoke(&fcinfo);

		/* Combine results per OR or AND semantics */
		if (fcinfo.isnull)
			resultnull = true;
		else if (useOr)
		{
			if (DatumGetBool(thisresult))
			{
				result = BoolGetDatum(true);
				resultnull = false;
				break;			/* needn't look at any more elements */
			}
		}
		else
		{
			if (!DatumGetBool(thisresult))
			{
				result = BoolGetDatum(false);
				resultnull = false;
				break;			/* needn't look at any more elements */
			}
		}
	}

	*isNull = resultnull;
	return result;
}

/* ----------------------------------------------------------------
 *		ExecEvalNot
 *		ExecEvalOr
 *		ExecEvalAnd
 *
 *		Evaluate boolean expressions, with appropriate short-circuiting.
 *
 *		The query planner reformulates clause expressions in the
 *		qualification to conjunctive normal form.  If we ever get
 *		an AND to evaluate, we can be sure that it's not a top-level
 *		clause in the qualification, but appears lower (as a function
 *		argument, for example), or in the target list.	Not that you
 *		need to know this, mind you...
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalNot(BoolExprState *notclause, ExprContext *econtext, bool *isNull)
{
	ExprState  *clause;
	Datum		expr_value;

	clause = lfirst(notclause->args);

	expr_value = ExecEvalExpr(clause, econtext, isNull, NULL);

	/*
	 * if the expression evaluates to null, then we just cascade the null
	 * back to whoever called us.
	 */
	if (*isNull)
		return expr_value;

	/*
	 * evaluation of 'not' is simple.. expr is false, then return 'true'
	 * and vice versa.
	 */
	return BoolGetDatum(!DatumGetBool(expr_value));
}

/* ----------------------------------------------------------------
 *		ExecEvalOr
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalOr(BoolExprState *orExpr, ExprContext *econtext, bool *isNull)
{
	List	   *clauses;
	List	   *clause;
	bool		AnyNull;
	Datum		clause_value;

	clauses = orExpr->args;
	AnyNull = false;

	/*
	 * If any of the clauses is TRUE, the OR result is TRUE regardless of
	 * the states of the rest of the clauses, so we can stop evaluating
	 * and return TRUE immediately.  If none are TRUE and one or more is
	 * NULL, we return NULL; otherwise we return FALSE.  This makes sense
	 * when you interpret NULL as "don't know": if we have a TRUE then the
	 * OR is TRUE even if we aren't sure about some of the other inputs.
	 * If all the known inputs are FALSE, but we have one or more "don't
	 * knows", then we have to report that we "don't know" what the OR's
	 * result should be --- perhaps one of the "don't knows" would have
	 * been TRUE if we'd known its value.  Only when all the inputs are
	 * known to be FALSE can we state confidently that the OR's result is
	 * FALSE.
	 */
	foreach(clause, clauses)
	{
		clause_value = ExecEvalExpr((ExprState *) lfirst(clause),
									econtext, isNull, NULL);

		/*
		 * if we have a non-null true result, then return it.
		 */
		if (*isNull)
			AnyNull = true;		/* remember we got a null */
		else if (DatumGetBool(clause_value))
			return clause_value;
	}

	/* AnyNull is true if at least one clause evaluated to NULL */
	*isNull = AnyNull;
	return BoolGetDatum(false);
}

/* ----------------------------------------------------------------
 *		ExecEvalAnd
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalAnd(BoolExprState *andExpr, ExprContext *econtext, bool *isNull)
{
	List	   *clauses;
	List	   *clause;
	bool		AnyNull;
	Datum		clause_value;

	clauses = andExpr->args;
	AnyNull = false;

	/*
	 * If any of the clauses is FALSE, the AND result is FALSE regardless
	 * of the states of the rest of the clauses, so we can stop evaluating
	 * and return FALSE immediately.  If none are FALSE and one or more is
	 * NULL, we return NULL; otherwise we return TRUE.	This makes sense
	 * when you interpret NULL as "don't know", using the same sort of
	 * reasoning as for OR, above.
	 */
	foreach(clause, clauses)
	{
		clause_value = ExecEvalExpr((ExprState *) lfirst(clause),
									econtext, isNull, NULL);

		/*
		 * if we have a non-null false result, then return it.
		 */
		if (*isNull)
			AnyNull = true;		/* remember we got a null */
		else if (!DatumGetBool(clause_value))
			return clause_value;
	}

	/* AnyNull is true if at least one clause evaluated to NULL */
	*isNull = AnyNull;
	return BoolGetDatum(!AnyNull);
}


/* ----------------------------------------------------------------
 *		ExecEvalCase
 *
 *		Evaluate a CASE clause. Will have boolean expressions
 *		inside the WHEN clauses, and will have expressions
 *		for results.
 *		- thomas 1998-11-09
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalCase(CaseExprState *caseExpr, ExprContext *econtext,
			 bool *isNull, ExprDoneCond *isDone)
{
	List	   *clauses;
	List	   *clause;
	Datum		clause_value;

	clauses = caseExpr->args;

	/*
	 * we evaluate each of the WHEN clauses in turn, as soon as one is
	 * true we return the corresponding result. If none are true then we
	 * return the value of the default clause, or NULL if there is none.
	 */
	foreach(clause, clauses)
	{
		CaseWhenState *wclause = lfirst(clause);

		clause_value = ExecEvalExpr(wclause->expr,
									econtext,
									isNull,
									NULL);

		/*
		 * if we have a true test, then we return the result, since the
		 * case statement is satisfied.  A NULL result from the test is
		 * not considered true.
		 */
		if (DatumGetBool(clause_value) && !*isNull)
		{
			return ExecEvalExpr(wclause->result,
								econtext,
								isNull,
								isDone);
		}
	}

	if (caseExpr->defresult)
	{
		return ExecEvalExpr(caseExpr->defresult,
							econtext,
							isNull,
							isDone);
	}

	*isNull = true;
	return (Datum) 0;
}

/* ----------------------------------------------------------------
 *		ExecEvalArray - ARRAY[] expressions
 *
 * NOTE: currently, if any input value is NULL then we return a NULL array,
 * so the ARRAY[] construct can be considered strict.  Eventually this will
 * change; when it does, be sure to fix contain_nonstrict_functions().
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalArray(ArrayExprState *astate, ExprContext *econtext,
			  bool *isNull)
{
	ArrayExpr  *arrayExpr = (ArrayExpr *) astate->xprstate.expr;
	ArrayType  *result;
	List	   *element;
	Oid			element_type = arrayExpr->element_typeid;
	int			ndims = 0;
	int			dims[MAXDIM];
	int			lbs[MAXDIM];

	if (!arrayExpr->multidims)
	{
		/* Elements are presumably of scalar type */
		int			nelems;
		Datum	   *dvalues;
		int			i = 0;

		ndims = 1;
		nelems = length(astate->elements);

		/* Shouldn't happen here, but if length is 0, return NULL */
		if (nelems == 0)
		{
			*isNull = true;
			return (Datum) 0;
		}

		dvalues = (Datum *) palloc(nelems * sizeof(Datum));

		/* loop through and build array of datums */
		foreach(element, astate->elements)
		{
			ExprState  *e = (ExprState *) lfirst(element);
			bool		eisnull;

			dvalues[i++] = ExecEvalExpr(e, econtext, &eisnull, NULL);
			if (eisnull)
			{
				*isNull = true;
				return (Datum) 0;
			}
		}

		/* setup for 1-D array of the given length */
		dims[0] = nelems;
		lbs[0] = 1;

		result = construct_md_array(dvalues, ndims, dims, lbs,
									element_type,
									astate->elemlength,
									astate->elembyval,
									astate->elemalign);
	}
	else
	{
		/* Must be nested array expressions */
		char	   *dat = NULL;
		Size		ndatabytes = 0;
		int			nbytes;
		int			outer_nelems = length(astate->elements);
		int			elem_ndims = 0;
		int		   *elem_dims = NULL;
		int		   *elem_lbs = NULL;
		bool		firstone = true;
		int			i;

		/* loop through and get data area from each element */
		foreach(element, astate->elements)
		{
			ExprState  *e = (ExprState *) lfirst(element);
			bool		eisnull;
			Datum		arraydatum;
			ArrayType  *array;
			int			elem_ndatabytes;

			arraydatum = ExecEvalExpr(e, econtext, &eisnull, NULL);
			if (eisnull)
			{
				*isNull = true;
				return (Datum) 0;
			}

			array = DatumGetArrayTypeP(arraydatum);

			/* run-time double-check on element type */
			if (element_type != ARR_ELEMTYPE(array))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("cannot merge incompatible arrays"),
						 errdetail("Array with element type %s cannot be "
								   "included in ARRAY construct with element type %s.",
								   format_type_be(ARR_ELEMTYPE(array)),
								   format_type_be(element_type))));

			if (firstone)
			{
				/* Get sub-array details from first member */
				elem_ndims = ARR_NDIM(array);
				ndims = elem_ndims + 1;
				if (ndims <= 0 || ndims > MAXDIM)
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("number of array dimensions (%d) exceeds " \
									"the maximum allowed (%d)", ndims, MAXDIM)));

				elem_dims = (int *) palloc(elem_ndims * sizeof(int));
				memcpy(elem_dims, ARR_DIMS(array), elem_ndims * sizeof(int));
				elem_lbs = (int *) palloc(elem_ndims * sizeof(int));
				memcpy(elem_lbs, ARR_LBOUND(array), elem_ndims * sizeof(int));

				firstone = false;
			}
			else
			{
				/* Check other sub-arrays are compatible */
				if (elem_ndims != ARR_NDIM(array) ||
					memcmp(elem_dims, ARR_DIMS(array),
						   elem_ndims * sizeof(int)) != 0 ||
					memcmp(elem_lbs, ARR_LBOUND(array),
						   elem_ndims * sizeof(int)) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						errmsg("multidimensional arrays must have array "
							   "expressions with matching dimensions")));
			}

			elem_ndatabytes = ARR_SIZE(array) - ARR_OVERHEAD(elem_ndims);
			ndatabytes += elem_ndatabytes;
			if (dat == NULL)
				dat = (char *) palloc(ndatabytes);
			else
				dat = (char *) repalloc(dat, ndatabytes);

			memcpy(dat + (ndatabytes - elem_ndatabytes),
				   ARR_DATA_PTR(array),
				   elem_ndatabytes);
		}

		/* setup for multi-D array */
		dims[0] = outer_nelems;
		lbs[0] = 1;
		for (i = 1; i < ndims; i++)
		{
			dims[i] = elem_dims[i - 1];
			lbs[i] = elem_lbs[i - 1];
		}

		nbytes = ndatabytes + ARR_OVERHEAD(ndims);
		result = (ArrayType *) palloc(nbytes);

		result->size = nbytes;
		result->ndim = ndims;
		result->flags = 0;
		result->elemtype = element_type;
		memcpy(ARR_DIMS(result), dims, ndims * sizeof(int));
		memcpy(ARR_LBOUND(result), lbs, ndims * sizeof(int));
		if (ndatabytes > 0)
			memcpy(ARR_DATA_PTR(result), dat, ndatabytes);

		if (dat != NULL)
			pfree(dat);
	}

	return PointerGetDatum(result);
}

/* ----------------------------------------------------------------
 *		ExecEvalCoalesce
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalCoalesce(CoalesceExprState *coalesceExpr, ExprContext *econtext,
				 bool *isNull)
{
	List	   *arg;

	/* Simply loop through until something NOT NULL is found */
	foreach(arg, coalesceExpr->args)
	{
		ExprState  *e = (ExprState *) lfirst(arg);
		Datum		value;

		value = ExecEvalExpr(e, econtext, isNull, NULL);
		if (!*isNull)
			return value;
	}

	/* Else return NULL */
	*isNull = true;
	return (Datum) 0;
}

/* ----------------------------------------------------------------
 *		ExecEvalNullIf
 *
 * Note that this is *always* derived from the equals operator,
 * but since we need special processing of the arguments
 * we can not simply reuse ExecEvalOper() or ExecEvalFunc().
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalNullIf(FuncExprState *fcache, ExprContext *econtext,
			   bool *isNull)
{
	Datum		result;
	FunctionCallInfoData fcinfo;
	ExprDoneCond argDone;
	List	   *argList;

	/*
	 * Initialize function cache if first time through
	 */
	if (fcache->func.fn_oid == InvalidOid)
	{
		NullIfExpr *op = (NullIfExpr *) fcache->xprstate.expr;

		init_fcache(op->opfuncid, fcache, econtext->ecxt_per_query_memory);
		Assert(!fcache->func.fn_retset);
	}

	/*
	 * extract info from fcache
	 */
	argList = fcache->args;

	/* Need to prep callinfo structure */
	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.flinfo = &(fcache->func);
	argDone = ExecEvalFuncArgs(&fcinfo, argList, econtext);
	if (argDone != ExprSingleResult)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("NULLIF does not support set arguments")));
	Assert(fcinfo.nargs == 2);

	/* if either argument is NULL they can't be equal */
	if (!fcinfo.argnull[0] && !fcinfo.argnull[1])
	{
		fcinfo.isnull = false;
		result = FunctionCallInvoke(&fcinfo);
		/* if the arguments are equal return null */
		if (!fcinfo.isnull && DatumGetBool(result))
		{
			*isNull = true;
			return (Datum) 0;
		}
	}

	/* else return first argument */
	*isNull = fcinfo.argnull[0];
	return fcinfo.arg[0];
}

/* ----------------------------------------------------------------
 *		ExecEvalNullTest
 *
 *		Evaluate a NullTest node.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalNullTest(GenericExprState *nstate,
				 ExprContext *econtext,
				 bool *isNull,
				 ExprDoneCond *isDone)
{
	NullTest   *ntest = (NullTest *) nstate->xprstate.expr;
	Datum		result;

	result = ExecEvalExpr(nstate->arg, econtext, isNull, isDone);

	if (isDone && *isDone == ExprEndResult)
		return result;			/* nothing to check */

	switch (ntest->nulltesttype)
	{
		case IS_NULL:
			if (*isNull)
			{
				*isNull = false;
				return BoolGetDatum(true);
			}
			else
				return BoolGetDatum(false);
		case IS_NOT_NULL:
			if (*isNull)
			{
				*isNull = false;
				return BoolGetDatum(false);
			}
			else
				return BoolGetDatum(true);
		default:
			elog(ERROR, "unrecognized nulltesttype: %d",
				 (int) ntest->nulltesttype);
			return (Datum) 0;	/* keep compiler quiet */
	}
}

/* ----------------------------------------------------------------
 *		ExecEvalBooleanTest
 *
 *		Evaluate a BooleanTest node.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalBooleanTest(GenericExprState *bstate,
					ExprContext *econtext,
					bool *isNull,
					ExprDoneCond *isDone)
{
	BooleanTest *btest = (BooleanTest *) bstate->xprstate.expr;
	Datum		result;

	result = ExecEvalExpr(bstate->arg, econtext, isNull, isDone);

	if (isDone && *isDone == ExprEndResult)
		return result;			/* nothing to check */

	switch (btest->booltesttype)
	{
		case IS_TRUE:
			if (*isNull)
			{
				*isNull = false;
				return BoolGetDatum(false);
			}
			else if (DatumGetBool(result))
				return BoolGetDatum(true);
			else
				return BoolGetDatum(false);
		case IS_NOT_TRUE:
			if (*isNull)
			{
				*isNull = false;
				return BoolGetDatum(true);
			}
			else if (DatumGetBool(result))
				return BoolGetDatum(false);
			else
				return BoolGetDatum(true);
		case IS_FALSE:
			if (*isNull)
			{
				*isNull = false;
				return BoolGetDatum(false);
			}
			else if (DatumGetBool(result))
				return BoolGetDatum(false);
			else
				return BoolGetDatum(true);
		case IS_NOT_FALSE:
			if (*isNull)
			{
				*isNull = false;
				return BoolGetDatum(true);
			}
			else if (DatumGetBool(result))
				return BoolGetDatum(true);
			else
				return BoolGetDatum(false);
		case IS_UNKNOWN:
			if (*isNull)
			{
				*isNull = false;
				return BoolGetDatum(true);
			}
			else
				return BoolGetDatum(false);
		case IS_NOT_UNKNOWN:
			if (*isNull)
			{
				*isNull = false;
				return BoolGetDatum(false);
			}
			else
				return BoolGetDatum(true);
		default:
			elog(ERROR, "unrecognized booltesttype: %d",
				 (int) btest->booltesttype);
			return (Datum) 0;	/* keep compiler quiet */
	}
}

/*
 * ExecEvalCoerceToDomain
 *
 * Test the provided data against the domain constraint(s).  If the data
 * passes the constraint specifications, pass it through (return the
 * datum) otherwise throw an error.
 */
static Datum
ExecEvalCoerceToDomain(CoerceToDomainState *cstate, ExprContext *econtext,
					   bool *isNull, ExprDoneCond *isDone)
{
	CoerceToDomain *ctest = (CoerceToDomain *) cstate->xprstate.expr;
	Datum		result;
	List	   *l;

	result = ExecEvalExpr(cstate->arg, econtext, isNull, isDone);

	if (isDone && *isDone == ExprEndResult)
		return result;			/* nothing to check */

	foreach(l, cstate->constraints)
	{
		DomainConstraintState *con = (DomainConstraintState *) lfirst(l);

		switch (con->constrainttype)
		{
			case DOM_CONSTRAINT_NOTNULL:
				if (*isNull)
					ereport(ERROR,
							(errcode(ERRCODE_NOT_NULL_VIOLATION),
						   errmsg("domain %s does not allow null values",
								  format_type_be(ctest->resulttype))));
				break;
			case DOM_CONSTRAINT_CHECK:
				{
					Datum		conResult;
					bool		conIsNull;
					Datum		save_datum;
					bool		save_isNull;

					/*
					 * Set up value to be returned by CoerceToDomainValue
					 * nodes. We must save and restore prior setting of
					 * econtext's domainValue fields, in case this node is
					 * itself within a check expression for another
					 * domain.
					 */
					save_datum = econtext->domainValue_datum;
					save_isNull = econtext->domainValue_isNull;

					econtext->domainValue_datum = result;
					econtext->domainValue_isNull = *isNull;

					conResult = ExecEvalExpr(con->check_expr,
											 econtext, &conIsNull, NULL);

					if (!conIsNull &&
						!DatumGetBool(conResult))
						ereport(ERROR,
								(errcode(ERRCODE_CHECK_VIOLATION),
								 errmsg("value for domain %s violates check constraint \"%s\"",
										format_type_be(ctest->resulttype),
										con->name)));
					econtext->domainValue_datum = save_datum;
					econtext->domainValue_isNull = save_isNull;

					break;
				}
			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 (int) con->constrainttype);
				break;
		}
	}

	/* If all has gone well (constraints did not fail) return the datum */
	return result;
}

/*
 * ExecEvalCoerceToDomainValue
 *
 * Return the value stored by CoerceToDomain.
 */
static Datum
ExecEvalCoerceToDomainValue(CoerceToDomainValue *conVal,
							ExprContext *econtext, bool *isNull)
{
	*isNull = econtext->domainValue_isNull;
	return econtext->domainValue_datum;
}

/* ----------------------------------------------------------------
 *		ExecEvalFieldSelect
 *
 *		Evaluate a FieldSelect node.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalFieldSelect(GenericExprState *fstate,
					ExprContext *econtext,
					bool *isNull,
					ExprDoneCond *isDone)
{
	FieldSelect *fselect = (FieldSelect *) fstate->xprstate.expr;
	Datum		result;
	TupleTableSlot *resSlot;

	result = ExecEvalExpr(fstate->arg, econtext, isNull, isDone);

	/* this test covers the isDone exception too: */
	if (*isNull)
		return result;

	resSlot = (TupleTableSlot *) DatumGetPointer(result);
	Assert(resSlot != NULL && IsA(resSlot, TupleTableSlot));
	result = heap_getattr(resSlot->val,
						  fselect->fieldnum,
						  resSlot->ttc_tupleDescriptor,
						  isNull);
	return result;
}

/* ----------------------------------------------------------------
 *		ExecEvalExpr
 *
 *		Recursively evaluate a targetlist or qualification expression.
 *
 * Inputs:
 *		expression: the expression state tree to evaluate
 *		econtext: evaluation context information
 *
 * Outputs:
 *		return value: Datum value of result
 *		*isNull: set to TRUE if result is NULL (actual return value is
 *				 meaningless if so); set to FALSE if non-null result
 *		*isDone: set to indicator of set-result status
 *
 * A caller that can only accept a singleton (non-set) result should pass
 * NULL for isDone; if the expression computes a set result then an error
 * will be reported via ereport.  If the caller does pass an isDone pointer
 * then *isDone is set to one of these three states:
 *		ExprSingleResult		singleton result (not a set)
 *		ExprMultipleResult		return value is one element of a set
 *		ExprEndResult			there are no more elements in the set
 * When ExprMultipleResult is returned, the caller should invoke
 * ExecEvalExpr() repeatedly until ExprEndResult is returned.  ExprEndResult
 * is returned after the last real set element.  For convenience isNull will
 * always be set TRUE when ExprEndResult is returned, but this should not be
 * taken as indicating a NULL element of the set.  Note that these return
 * conventions allow us to distinguish among a singleton NULL, a NULL element
 * of a set, and an empty set.
 *
 * The caller should already have switched into the temporary memory
 * context econtext->ecxt_per_tuple_memory.  The convenience entry point
 * ExecEvalExprSwitchContext() is provided for callers who don't prefer to
 * do the switch in an outer loop.	We do not do the switch here because
 * it'd be a waste of cycles during recursive entries to ExecEvalExpr().
 *
 * This routine is an inner loop routine and must be as fast as possible.
 * ----------------------------------------------------------------
 */
Datum
ExecEvalExpr(ExprState *expression,
			 ExprContext *econtext,
			 bool *isNull,
			 ExprDoneCond *isDone)
{
	Datum		retDatum;
	Expr	   *expr;

	/* Set default values for result flags: non-null, not a set result */
	*isNull = false;
	if (isDone)
		*isDone = ExprSingleResult;

	/* Is this still necessary?  Doubtful... */
	if (expression == NULL)
	{
		*isNull = true;
		return (Datum) 0;
	}

	/*
	 * here we dispatch the work to the appropriate type of function given
	 * the type of our expression.
	 */
	expr = expression->expr;
	switch (nodeTag(expr))
	{
		case T_Var:
			retDatum = ExecEvalVar((Var *) expr, econtext, isNull);
			break;
		case T_Const:
			{
				Const	   *con = (Const *) expr;

				retDatum = con->constvalue;
				*isNull = con->constisnull;
				break;
			}
		case T_Param:
			retDatum = ExecEvalParam((Param *) expr, econtext, isNull);
			break;
		case T_Aggref:
			retDatum = ExecEvalAggref((AggrefExprState *) expression,
									  econtext,
									  isNull);
			break;
		case T_ArrayRef:
			retDatum = ExecEvalArrayRef((ArrayRefExprState *) expression,
										econtext,
										isNull,
										isDone);
			break;
		case T_FuncExpr:
			retDatum = ExecEvalFunc((FuncExprState *) expression, econtext,
									isNull, isDone);
			break;
		case T_OpExpr:
			retDatum = ExecEvalOper((FuncExprState *) expression, econtext,
									isNull, isDone);
			break;
		case T_DistinctExpr:
			retDatum = ExecEvalDistinct((FuncExprState *) expression, econtext,
										isNull);
			break;
		case T_ScalarArrayOpExpr:
			retDatum = ExecEvalScalarArrayOp((ScalarArrayOpExprState *) expression,
											 econtext, isNull);
			break;
		case T_BoolExpr:
			{
				BoolExprState *state = (BoolExprState *) expression;

				switch (((BoolExpr *) expr)->boolop)
				{
					case AND_EXPR:
						retDatum = ExecEvalAnd(state, econtext, isNull);
						break;
					case OR_EXPR:
						retDatum = ExecEvalOr(state, econtext, isNull);
						break;
					case NOT_EXPR:
						retDatum = ExecEvalNot(state, econtext, isNull);
						break;
					default:
						elog(ERROR, "unrecognized boolop: %d",
							 (int) ((BoolExpr *) expr)->boolop);
						retDatum = 0;	/* keep compiler quiet */
						break;
				}
				break;
			}
		case T_SubPlan:
			retDatum = ExecSubPlan((SubPlanState *) expression,
								   econtext,
								   isNull);
			break;
		case T_FieldSelect:
			retDatum = ExecEvalFieldSelect((GenericExprState *) expression,
										   econtext,
										   isNull,
										   isDone);
			break;
		case T_RelabelType:
			retDatum = ExecEvalExpr(((GenericExprState *) expression)->arg,
									econtext,
									isNull,
									isDone);
			break;
		case T_CaseExpr:
			retDatum = ExecEvalCase((CaseExprState *) expression,
									econtext,
									isNull,
									isDone);
			break;
		case T_ArrayExpr:
			retDatum = ExecEvalArray((ArrayExprState *) expression,
									 econtext,
									 isNull);
			break;
		case T_CoalesceExpr:
			retDatum = ExecEvalCoalesce((CoalesceExprState *) expression,
										econtext,
										isNull);
			break;
		case T_NullIfExpr:
			retDatum = ExecEvalNullIf((FuncExprState *) expression,
									  econtext,
									  isNull);
			break;
		case T_NullTest:
			retDatum = ExecEvalNullTest((GenericExprState *) expression,
										econtext,
										isNull,
										isDone);
			break;
		case T_BooleanTest:
			retDatum = ExecEvalBooleanTest((GenericExprState *) expression,
										   econtext,
										   isNull,
										   isDone);
			break;
		case T_CoerceToDomain:
			retDatum = ExecEvalCoerceToDomain((CoerceToDomainState *) expression,
											  econtext,
											  isNull,
											  isDone);
			break;
		case T_CoerceToDomainValue:
			retDatum = ExecEvalCoerceToDomainValue((CoerceToDomainValue *) expr,
												   econtext,
												   isNull);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(expression));
			retDatum = 0;		/* keep compiler quiet */
			break;
	}

	return retDatum;
}	/* ExecEvalExpr() */


/*
 * Same as above, but get into the right allocation context explicitly.
 */
Datum
ExecEvalExprSwitchContext(ExprState *expression,
						  ExprContext *econtext,
						  bool *isNull,
						  ExprDoneCond *isDone)
{
	Datum		retDatum;
	MemoryContext oldContext;

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	retDatum = ExecEvalExpr(expression, econtext, isNull, isDone);
	MemoryContextSwitchTo(oldContext);
	return retDatum;
}


/*
 * ExecInitExpr: prepare an expression tree for execution
 *
 * This function builds and returns an ExprState tree paralleling the given
 * Expr node tree.	The ExprState tree can then be handed to ExecEvalExpr
 * for execution.  Because the Expr tree itself is read-only as far as
 * ExecInitExpr and ExecEvalExpr are concerned, several different executions
 * of the same plan tree can occur concurrently.
 *
 * This must be called in a memory context that will last as long as repeated
 * executions of the expression are needed.  Typically the context will be
 * the same as the per-query context of the associated ExprContext.
 *
 * Any Aggref and SubPlan nodes found in the tree are added to the lists
 * of such nodes held by the parent PlanState.	Otherwise, we do very little
 * initialization here other than building the state-node tree.  Any nontrivial
 * work associated with initializing runtime info for a node should happen
 * during the first actual evaluation of that node.  (This policy lets us
 * avoid work if the node is never actually evaluated.)
 *
 * Note: there is no ExecEndExpr function; we assume that any resource
 * cleanup needed will be handled by just releasing the memory context
 * in which the state tree is built.  Functions that require additional
 * cleanup work can register a shutdown callback in the ExprContext.
 *
 *	'node' is the root of the expression tree to examine
 *	'parent' is the PlanState node that owns the expression.
 *
 * 'parent' may be NULL if we are preparing an expression that is not
 * associated with a plan tree.  (If so, it can't have aggs or subplans.)
 * This case should usually come through ExecPrepareExpr, not directly here.
 */
ExprState *
ExecInitExpr(Expr *node, PlanState *parent)
{
	ExprState  *state;

	if (node == NULL)
		return NULL;
	switch (nodeTag(node))
	{
		case T_Var:
		case T_Const:
		case T_Param:
		case T_CoerceToDomainValue:
			/* No special setup needed for these node types */
			state = (ExprState *) makeNode(ExprState);
			break;
		case T_Aggref:
			{
				Aggref	   *aggref = (Aggref *) node;
				AggrefExprState *astate = makeNode(AggrefExprState);

				if (parent && IsA(parent, AggState))
				{
					AggState   *aggstate = (AggState *) parent;
					int			naggs;

					aggstate->aggs = lcons(astate, aggstate->aggs);
					naggs = ++aggstate->numaggs;

					astate->target = ExecInitExpr(aggref->target, parent);

					/*
					 * Complain if the aggregate's argument contains any
					 * aggregates; nested agg functions are semantically
					 * nonsensical.  (This should have been caught
					 * earlier, but we defend against it here anyway.)
					 */
					if (naggs != aggstate->numaggs)
						ereport(ERROR,
								(errcode(ERRCODE_GROUPING_ERROR),
								 errmsg("aggregate function calls may not be nested")));
				}
				else
				{
					/* planner messed up */
					elog(ERROR, "aggref found in non-Agg plan node");
				}
				state = (ExprState *) astate;
			}
			break;
		case T_ArrayRef:
			{
				ArrayRef   *aref = (ArrayRef *) node;
				ArrayRefExprState *astate = makeNode(ArrayRefExprState);

				astate->refupperindexpr = (List *)
					ExecInitExpr((Expr *) aref->refupperindexpr, parent);
				astate->reflowerindexpr = (List *)
					ExecInitExpr((Expr *) aref->reflowerindexpr, parent);
				astate->refexpr = ExecInitExpr(aref->refexpr, parent);
				astate->refassgnexpr = ExecInitExpr(aref->refassgnexpr,
													parent);
				/* do one-time catalog lookups for type info */
				astate->refattrlength = get_typlen(aref->refarraytype);
				get_typlenbyvalalign(aref->refelemtype,
									 &astate->refelemlength,
									 &astate->refelembyval,
									 &astate->refelemalign);
				state = (ExprState *) astate;
			}
			break;
		case T_FuncExpr:
			{
				FuncExpr   *funcexpr = (FuncExpr *) node;
				FuncExprState *fstate = makeNode(FuncExprState);

				fstate->args = (List *)
					ExecInitExpr((Expr *) funcexpr->args, parent);
				fstate->func.fn_oid = InvalidOid;		/* not initialized */
				state = (ExprState *) fstate;
			}
			break;
		case T_OpExpr:
			{
				OpExpr	   *opexpr = (OpExpr *) node;
				FuncExprState *fstate = makeNode(FuncExprState);

				fstate->args = (List *)
					ExecInitExpr((Expr *) opexpr->args, parent);
				fstate->func.fn_oid = InvalidOid;		/* not initialized */
				state = (ExprState *) fstate;
			}
			break;
		case T_DistinctExpr:
			{
				DistinctExpr *distinctexpr = (DistinctExpr *) node;
				FuncExprState *fstate = makeNode(FuncExprState);

				fstate->args = (List *)
					ExecInitExpr((Expr *) distinctexpr->args, parent);
				fstate->func.fn_oid = InvalidOid;		/* not initialized */
				state = (ExprState *) fstate;
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *opexpr = (ScalarArrayOpExpr *) node;
				ScalarArrayOpExprState *sstate = makeNode(ScalarArrayOpExprState);

				sstate->fxprstate.args = (List *)
					ExecInitExpr((Expr *) opexpr->args, parent);
				sstate->fxprstate.func.fn_oid = InvalidOid;		/* not initialized */
				sstate->element_type = InvalidOid;		/* ditto */
				state = (ExprState *) sstate;
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *boolexpr = (BoolExpr *) node;
				BoolExprState *bstate = makeNode(BoolExprState);

				bstate->args = (List *)
					ExecInitExpr((Expr *) boolexpr->args, parent);
				state = (ExprState *) bstate;
			}
			break;
		case T_SubPlan:
			{
				/* Keep this in sync with ExecInitExprInitPlan, below */
				SubPlan    *subplan = (SubPlan *) node;
				SubPlanState *sstate = makeNode(SubPlanState);

				if (!parent)
					elog(ERROR, "SubPlan found with no parent plan");

				/*
				 * Here we just add the SubPlanState nodes to
				 * parent->subPlan.  The subplans will be initialized
				 * later.
				 */
				parent->subPlan = lcons(sstate, parent->subPlan);
				sstate->sub_estate = NULL;
				sstate->planstate = NULL;

				sstate->exprs = (List *)
					ExecInitExpr((Expr *) subplan->exprs, parent);
				sstate->args = (List *)
					ExecInitExpr((Expr *) subplan->args, parent);

				state = (ExprState *) sstate;
			}
			break;
		case T_FieldSelect:
			{
				FieldSelect *fselect = (FieldSelect *) node;
				GenericExprState *gstate = makeNode(GenericExprState);

				gstate->arg = ExecInitExpr(fselect->arg, parent);
				state = (ExprState *) gstate;
			}
			break;
		case T_RelabelType:
			{
				RelabelType *relabel = (RelabelType *) node;
				GenericExprState *gstate = makeNode(GenericExprState);

				gstate->arg = ExecInitExpr(relabel->arg, parent);
				state = (ExprState *) gstate;
			}
			break;
		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;
				CaseExprState *cstate = makeNode(CaseExprState);
				FastList	outlist;
				List	   *inlist;

				FastListInit(&outlist);
				foreach(inlist, caseexpr->args)
				{
					CaseWhen   *when = (CaseWhen *) lfirst(inlist);
					CaseWhenState *wstate = makeNode(CaseWhenState);

					Assert(IsA(when, CaseWhen));
					wstate->xprstate.expr = (Expr *) when;
					wstate->expr = ExecInitExpr(when->expr, parent);
					wstate->result = ExecInitExpr(when->result, parent);
					FastAppend(&outlist, wstate);
				}
				cstate->args = FastListValue(&outlist);
				/* caseexpr->arg should be null by now */
				Assert(caseexpr->arg == NULL);
				cstate->defresult = ExecInitExpr(caseexpr->defresult, parent);
				state = (ExprState *) cstate;
			}
			break;
		case T_ArrayExpr:
			{
				ArrayExpr  *arrayexpr = (ArrayExpr *) node;
				ArrayExprState *astate = makeNode(ArrayExprState);
				FastList	outlist;
				List	   *inlist;

				FastListInit(&outlist);
				foreach(inlist, arrayexpr->elements)
				{
					Expr	   *e = (Expr *) lfirst(inlist);
					ExprState  *estate;

					estate = ExecInitExpr(e, parent);
					FastAppend(&outlist, estate);
				}
				astate->elements = FastListValue(&outlist);
				/* do one-time catalog lookup for type info */
				get_typlenbyvalalign(arrayexpr->element_typeid,
									 &astate->elemlength,
									 &astate->elembyval,
									 &astate->elemalign);
				state = (ExprState *) astate;
			}
			break;
		case T_CoalesceExpr:
			{
				CoalesceExpr *coalesceexpr = (CoalesceExpr *) node;
				CoalesceExprState *cstate = makeNode(CoalesceExprState);
				FastList	outlist;
				List	   *inlist;

				FastListInit(&outlist);
				foreach(inlist, coalesceexpr->args)
				{
					Expr	   *e = (Expr *) lfirst(inlist);
					ExprState  *estate;

					estate = ExecInitExpr(e, parent);
					FastAppend(&outlist, estate);
				}
				cstate->args = FastListValue(&outlist);
				state = (ExprState *) cstate;
			}
			break;
		case T_NullIfExpr:
			{
				NullIfExpr *nullifexpr = (NullIfExpr *) node;
				FuncExprState *fstate = makeNode(FuncExprState);

				fstate->args = (List *)
					ExecInitExpr((Expr *) nullifexpr->args, parent);
				fstate->func.fn_oid = InvalidOid;		/* not initialized */
				state = (ExprState *) fstate;
			}
			break;
		case T_NullTest:
			{
				NullTest   *ntest = (NullTest *) node;
				GenericExprState *gstate = makeNode(GenericExprState);

				gstate->arg = ExecInitExpr(ntest->arg, parent);
				state = (ExprState *) gstate;
			}
			break;
		case T_BooleanTest:
			{
				BooleanTest *btest = (BooleanTest *) node;
				GenericExprState *gstate = makeNode(GenericExprState);

				gstate->arg = ExecInitExpr(btest->arg, parent);
				state = (ExprState *) gstate;
			}
			break;
		case T_CoerceToDomain:
			{
				CoerceToDomain *ctest = (CoerceToDomain *) node;
				CoerceToDomainState *cstate = makeNode(CoerceToDomainState);

				cstate->arg = ExecInitExpr(ctest->arg, parent);
				cstate->constraints = GetDomainConstraints(ctest->resulttype);
				state = (ExprState *) cstate;
			}
			break;
		case T_TargetEntry:
			{
				TargetEntry *tle = (TargetEntry *) node;
				GenericExprState *gstate = makeNode(GenericExprState);

				gstate->arg = ExecInitExpr(tle->expr, parent);
				state = (ExprState *) gstate;
			}
			break;
		case T_List:
			{
				FastList	outlist;
				List	   *inlist;

				FastListInit(&outlist);
				foreach(inlist, (List *) node)
				{
					FastAppend(&outlist,
							   ExecInitExpr((Expr *) lfirst(inlist),
											parent));
				}
				/* Don't fall through to the "common" code below */
				return (ExprState *) FastListValue(&outlist);
			}
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(node));
			state = NULL;		/* keep compiler quiet */
			break;
	}

	/* Common code for all state-node types */
	state->expr = node;

	return state;
}

/*
 * ExecInitExprInitPlan --- initialize a subplan expr that's being handled
 * as an InitPlan.	This is identical to ExecInitExpr's handling of a regular
 * subplan expr, except we do NOT want to add the node to the parent's
 * subplan list.
 */
SubPlanState *
ExecInitExprInitPlan(SubPlan *node, PlanState *parent)
{
	SubPlanState *sstate = makeNode(SubPlanState);

	if (!parent)
		elog(ERROR, "SubPlan found with no parent plan");

	/* The subplan's state will be initialized later */
	sstate->sub_estate = NULL;
	sstate->planstate = NULL;

	sstate->exprs = (List *) ExecInitExpr((Expr *) node->exprs, parent);
	sstate->args = (List *) ExecInitExpr((Expr *) node->args, parent);

	sstate->xprstate.expr = (Expr *) node;

	return sstate;
}

/*
 * ExecPrepareExpr --- initialize for expression execution outside a normal
 * Plan tree context.
 *
 * This differs from ExecInitExpr in that we don't assume the caller is
 * already running in the EState's per-query context.  Also, we apply
 * fix_opfuncids() to the passed expression tree to be sure it is ready
 * to run.	(In ordinary Plan trees the planner will have fixed opfuncids,
 * but callers outside the executor will not have done this.)
 */
ExprState *
ExecPrepareExpr(Expr *node, EState *estate)
{
	ExprState  *result;
	MemoryContext oldcontext;

	fix_opfuncids((Node *) node);

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	result = ExecInitExpr(node, NULL);

	MemoryContextSwitchTo(oldcontext);

	return result;
}


/* ----------------------------------------------------------------
 *					 ExecQual / ExecTargetList / ExecProject
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecQual
 *
 *		Evaluates a conjunctive boolean expression (qual list) and
 *		returns true iff none of the subexpressions are false.
 *		(We also return true if the list is empty.)
 *
 *	If some of the subexpressions yield NULL but none yield FALSE,
 *	then the result of the conjunction is NULL (ie, unknown)
 *	according to three-valued boolean logic.  In this case,
 *	we return the value specified by the "resultForNull" parameter.
 *
 *	Callers evaluating WHERE clauses should pass resultForNull=FALSE,
 *	since SQL specifies that tuples with null WHERE results do not
 *	get selected.  On the other hand, callers evaluating constraint
 *	conditions should pass resultForNull=TRUE, since SQL also specifies
 *	that NULL constraint conditions are not failures.
 *
 *	NOTE: it would not be correct to use this routine to evaluate an
 *	AND subclause of a boolean expression; for that purpose, a NULL
 *	result must be returned as NULL so that it can be properly treated
 *	in the next higher operator (cf. ExecEvalAnd and ExecEvalOr).
 *	This routine is only used in contexts where a complete expression
 *	is being evaluated and we know that NULL can be treated the same
 *	as one boolean result or the other.
 *
 * ----------------------------------------------------------------
 */
bool
ExecQual(List *qual, ExprContext *econtext, bool resultForNull)
{
	bool		result;
	MemoryContext oldContext;
	List	   *qlist;

	/*
	 * debugging stuff
	 */
	EV_printf("ExecQual: qual is ");
	EV_nodeDisplay(qual);
	EV_printf("\n");

	IncrProcessed();

	/*
	 * Run in short-lived per-tuple context while computing expressions.
	 */
	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/*
	 * Evaluate the qual conditions one at a time.	If we find a FALSE
	 * result, we can stop evaluating and return FALSE --- the AND result
	 * must be FALSE.  Also, if we find a NULL result when resultForNull
	 * is FALSE, we can stop and return FALSE --- the AND result must be
	 * FALSE or NULL in that case, and the caller doesn't care which.
	 *
	 * If we get to the end of the list, we can return TRUE.  This will
	 * happen when the AND result is indeed TRUE, or when the AND result
	 * is NULL (one or more NULL subresult, with all the rest TRUE) and
	 * the caller has specified resultForNull = TRUE.
	 */
	result = true;

	foreach(qlist, qual)
	{
		ExprState  *clause = (ExprState *) lfirst(qlist);
		Datum		expr_value;
		bool		isNull;

		expr_value = ExecEvalExpr(clause, econtext, &isNull, NULL);

		if (isNull)
		{
			if (resultForNull == false)
			{
				result = false; /* treat NULL as FALSE */
				break;
			}
		}
		else
		{
			if (!DatumGetBool(expr_value))
			{
				result = false; /* definitely FALSE */
				break;
			}
		}
	}

	MemoryContextSwitchTo(oldContext);

	return result;
}

/*
 * Number of items in a tlist (including any resjunk items!)
 */
int
ExecTargetListLength(List *targetlist)
{
	/* This used to be more complex, but fjoins are dead */
	return length(targetlist);
}

/*
 * Number of items in a tlist, not including any resjunk items
 */
int
ExecCleanTargetListLength(List *targetlist)
{
	int			len = 0;
	List	   *tl;

	foreach(tl, targetlist)
	{
		TargetEntry *curTle = (TargetEntry *) lfirst(tl);

		Assert(IsA(curTle, TargetEntry));
		if (!curTle->resdom->resjunk)
			len++;
	}
	return len;
}

/* ----------------------------------------------------------------
 *		ExecTargetList
 *
 *		Evaluates a targetlist with respect to the given
 *		expression context and returns a tuple.
 *
 * The caller must pass workspace for the values and nulls arrays
 * as well as the itemIsDone array.  This convention saves palloc'ing
 * workspace on each call, and some callers may find it useful to examine
 * the values array directly.
 *
 * As with ExecEvalExpr, the caller should pass isDone = NULL if not
 * prepared to deal with sets of result tuples.  Otherwise, a return
 * of *isDone = ExprMultipleResult signifies a set element, and a return
 * of *isDone = ExprEndResult signifies end of the set of tuple.
 * ----------------------------------------------------------------
 */
static HeapTuple
ExecTargetList(List *targetlist,
			   TupleDesc targettype,
			   ExprContext *econtext,
			   Datum *values,
			   char *nulls,
			   ExprDoneCond *itemIsDone,
			   ExprDoneCond *isDone)
{
	MemoryContext oldContext;
	List	   *tl;
	bool		isNull;
	bool		haveDoneSets;
	static struct tupleDesc NullTupleDesc;		/* we assume this inits to
												 * zeroes */

	/*
	 * debugging stuff
	 */
	EV_printf("ExecTargetList: tl is ");
	EV_nodeDisplay(targetlist);
	EV_printf("\n");

	/*
	 * Run in short-lived per-tuple context while computing expressions.
	 */
	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/*
	 * There used to be some klugy and demonstrably broken code here that
	 * special-cased the situation where targetlist == NIL.  Now we just
	 * fall through and return an empty-but-valid tuple.  We do, however,
	 * have to cope with the possibility that targettype is NULL ---
	 * heap_formtuple won't like that, so pass a dummy descriptor with
	 * natts = 0 to deal with it.
	 */
	if (targettype == NULL)
		targettype = &NullTupleDesc;

	/*
	 * evaluate all the expressions in the target list
	 */
	if (isDone)
		*isDone = ExprSingleResult;		/* until proven otherwise */

	haveDoneSets = false;		/* any exhausted set exprs in tlist? */

	foreach(tl, targetlist)
	{
		GenericExprState *gstate = (GenericExprState *) lfirst(tl);
		TargetEntry *tle = (TargetEntry *) gstate->xprstate.expr;
		AttrNumber	resind = tle->resdom->resno - 1;

		values[resind] = ExecEvalExpr(gstate->arg,
									  econtext,
									  &isNull,
									  &itemIsDone[resind]);
		nulls[resind] = isNull ? 'n' : ' ';

		if (itemIsDone[resind] != ExprSingleResult)
		{
			/* We have a set-valued expression in the tlist */
			if (isDone == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("set-valued function called in context that cannot accept a set")));
			if (itemIsDone[resind] == ExprMultipleResult)
			{
				/* we have undone sets in the tlist, set flag */
				*isDone = ExprMultipleResult;
			}
			else
			{
				/* we have done sets in the tlist, set flag for that */
				haveDoneSets = true;
			}
		}
	}

	if (haveDoneSets)
	{
		/*
		 * note: can't get here unless we verified isDone != NULL
		 */
		if (*isDone == ExprSingleResult)
		{
			/*
			 * all sets are done, so report that tlist expansion is
			 * complete.
			 */
			*isDone = ExprEndResult;
			MemoryContextSwitchTo(oldContext);
			return NULL;
		}
		else
		{
			/*
			 * We have some done and some undone sets.	Restart the done
			 * ones so that we can deliver a tuple (if possible).
			 */
			foreach(tl, targetlist)
			{
				GenericExprState *gstate = (GenericExprState *) lfirst(tl);
				TargetEntry *tle = (TargetEntry *) gstate->xprstate.expr;
				AttrNumber	resind = tle->resdom->resno - 1;

				if (itemIsDone[resind] == ExprEndResult)
				{
					values[resind] = ExecEvalExpr(gstate->arg,
												  econtext,
												  &isNull,
												  &itemIsDone[resind]);
					nulls[resind] = isNull ? 'n' : ' ';

					if (itemIsDone[resind] == ExprEndResult)
					{
						/*
						 * Oh dear, this item is returning an empty set.
						 * Guess we can't make a tuple after all.
						 */
						*isDone = ExprEndResult;
						break;
					}
				}
			}

			/*
			 * If we cannot make a tuple because some sets are empty, we
			 * still have to cycle the nonempty sets to completion, else
			 * resources will not be released from subplans etc.
			 *
			 * XXX is that still necessary?
			 */
			if (*isDone == ExprEndResult)
			{
				foreach(tl, targetlist)
				{
					GenericExprState *gstate = (GenericExprState *) lfirst(tl);
					TargetEntry *tle = (TargetEntry *) gstate->xprstate.expr;
					AttrNumber	resind = tle->resdom->resno - 1;

					while (itemIsDone[resind] == ExprMultipleResult)
					{
						(void) ExecEvalExpr(gstate->arg,
											econtext,
											&isNull,
											&itemIsDone[resind]);
					}
				}

				MemoryContextSwitchTo(oldContext);
				return NULL;
			}
		}
	}

	/*
	 * form the new result tuple (in the caller's memory context!)
	 */
	MemoryContextSwitchTo(oldContext);

	return heap_formtuple(targettype, values, nulls);
}

/* ----------------------------------------------------------------
 *		ExecProject
 *
 *		projects a tuple based on projection info and stores
 *		it in the specified tuple table slot.
 *
 *		Note: someday soon the executor can be extended to eliminate
 *			  redundant projections by storing pointers to datums
 *			  in the tuple table and then passing these around when
 *			  possible.  this should make things much quicker.
 *			  -cim 6/3/91
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecProject(ProjectionInfo *projInfo, ExprDoneCond *isDone)
{
	TupleTableSlot *slot;
	TupleDesc	tupType;
	HeapTuple	newTuple;

	/*
	 * sanity checks
	 */
	if (projInfo == NULL)
		return (TupleTableSlot *) NULL;

	/*
	 * get the projection info we want
	 */
	slot = projInfo->pi_slot;
	tupType = slot->ttc_tupleDescriptor;

	/*
	 * form a new result tuple (if possible --- result can be NULL)
	 */
	newTuple = ExecTargetList(projInfo->pi_targetlist,
							  tupType,
							  projInfo->pi_exprContext,
							  projInfo->pi_tupValues,
							  projInfo->pi_tupNulls,
							  projInfo->pi_itemIsDone,
							  isDone);

	/*
	 * store the tuple in the projection slot and return the slot.
	 */
	return ExecStoreTuple(newTuple,		/* tuple to store */
						  slot, /* slot to store in */
						  InvalidBuffer,		/* tuple has no buffer */
						  true);
}
