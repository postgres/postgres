/*-------------------------------------------------------------------------
 *
 * execQual.c
 *	  Routines to evaluate qualification and targetlist expressions
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execQual.c,v 1.81 2000/11/12 00:36:57 tgl Exp $
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
#include "executor/execFlatten.h"
#include "executor/execdebug.h"
#include "executor/functions.h"
#include "executor/nodeSubplan.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fcache.h"


/* static function decls */
static Datum ExecEvalAggref(Aggref *aggref, ExprContext *econtext,
							bool *isNull);
static Datum ExecEvalArrayRef(ArrayRef *arrayRef, ExprContext *econtext,
							  bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalVar(Var *variable, ExprContext *econtext, bool *isNull);
static Datum ExecEvalOper(Expr *opClause, ExprContext *econtext,
						  bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalFunc(Expr *funcClause, ExprContext *econtext,
						  bool *isNull, ExprDoneCond *isDone);
static ExprDoneCond ExecEvalFuncArgs(FunctionCachePtr fcache,
									 List *argList,
									 ExprContext *econtext);
static Datum ExecEvalNot(Expr *notclause, ExprContext *econtext, bool *isNull);
static Datum ExecEvalAnd(Expr *andExpr, ExprContext *econtext, bool *isNull);
static Datum ExecEvalOr(Expr *orExpr, ExprContext *econtext, bool *isNull);
static Datum ExecEvalCase(CaseExpr *caseExpr, ExprContext *econtext,
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
 * very desirable.  By returning the source array we make the assignment
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
ExecEvalArrayRef(ArrayRef *arrayRef,
				 ExprContext *econtext,
				 bool *isNull,
				 ExprDoneCond *isDone)
{
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
			DatumGetPointer(ExecEvalExpr(arrayRef->refexpr,
										 econtext,
										 isNull,
										 isDone));
		/*
		 * If refexpr yields NULL, result is always NULL, for now anyway.
		 * (This means you cannot assign to an element or slice of an array
		 * that's NULL; it'll just stay NULL.)
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

	foreach(elt, arrayRef->refupperindexpr)
	{
		if (i >= MAXDIM)
			elog(ERROR, "ExecEvalArrayRef: can only handle %d dimensions",
				 MAXDIM);

		upper.indx[i++] = DatumGetInt32(ExecEvalExpr((Node *) lfirst(elt),
													 econtext,
													 isNull,
													 NULL));
		/* If any index expr yields NULL, result is NULL or source array */
		if (*isNull)
		{
			if (! isAssignment || array_source == NULL)
				return (Datum) NULL;
			*isNull = false;
			return PointerGetDatum(array_source);
		}
	}

	if (arrayRef->reflowerindexpr != NIL)
	{
		foreach(elt, arrayRef->reflowerindexpr)
		{
			if (j >= MAXDIM)
				elog(ERROR, "ExecEvalArrayRef: can only handle %d dimensions",
					 MAXDIM);

			lower.indx[j++] = DatumGetInt32(ExecEvalExpr((Node *) lfirst(elt),
														 econtext,
														 isNull,
														 NULL));
			/* If any index expr yields NULL, result is NULL or source array */
			if (*isNull)
			{
				if (! isAssignment || array_source == NULL)
					return (Datum) NULL;
				*isNull = false;
				return PointerGetDatum(array_source);
			}
		}
		if (i != j)
			elog(ERROR,
				 "ExecEvalArrayRef: upper and lower indices mismatch");
		lIndex = lower.indx;
	}
	else
		lIndex = NULL;

	if (isAssignment)
	{
		Datum		sourceData = ExecEvalExpr(arrayRef->refassgnexpr,
											  econtext,
											  isNull,
											  NULL);
		/*
		 * For now, can't cope with inserting NULL into an array,
		 * so make it a no-op per discussion above...
		 */
		if (*isNull)
		{
			if (array_source == NULL)
				return (Datum) NULL;
			*isNull = false;
			return PointerGetDatum(array_source);
		}

		if (array_source == NULL)
			return sourceData;		/* XXX do something else? */

		if (lIndex == NULL)
			resultArray = array_set(array_source, i,
									upper.indx,
									sourceData,
									arrayRef->refelembyval,
									arrayRef->refelemlength,
									arrayRef->refattrlength,
									isNull);
		else
			resultArray = array_set_slice(array_source, i,
										  upper.indx, lower.indx,
										  (ArrayType *) DatumGetPointer(sourceData),
										  arrayRef->refelembyval,
										  arrayRef->refelemlength,
										  arrayRef->refattrlength,
										  isNull);
		return PointerGetDatum(resultArray);
	}

	if (lIndex == NULL)
		return array_ref(array_source, i,
						 upper.indx,
						 arrayRef->refelembyval,
						 arrayRef->refelemlength,
						 arrayRef->refattrlength,
						 isNull);
	else
	{
		resultArray = array_get_slice(array_source, i,
									  upper.indx, lower.indx,
									  arrayRef->refelembyval,
									  arrayRef->refelemlength,
									  arrayRef->refattrlength,
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
ExecEvalAggref(Aggref *aggref, ExprContext *econtext, bool *isNull)
{
	if (econtext->ecxt_aggvalues == NULL)		/* safety check */
		elog(ERROR, "ExecEvalAggref: no aggregates in this expression context");

	*isNull = econtext->ecxt_aggnulls[aggref->aggno];
	return econtext->ecxt_aggvalues[aggref->aggno];
}

/* ----------------------------------------------------------------
 *		ExecEvalVar
 *
 *		Returns a Datum whose value is the value of a range
 *		variable with respect to given expression context.
 *
 *
 *		As an entry condition, we expect that the datatype the
 *		plan expects to get (as told by our "variable" argument) is in
 *		fact the datatype of the attribute the plan says to fetch (as
 *		seen in the current context, identified by our "econtext"
 *		argument).
 *
 *		If we fetch a Type A attribute and Caller treats it as if it
 *		were Type B, there will be undefined results (e.g. crash).
 *		One way these might mismatch now is that we're accessing a
 *		catalog class and the type information in the pg_attribute
 *		class does not match the hardcoded pg_attribute information
 *		(in pg_attribute.h) for the class in question.
 *
 *		We have an Assert to make sure this entry condition is met.
 *
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

	/* (See prolog for explanation of this Assert) */
	Assert(attnum <= 0 ||
		   (attnum - 1 <= tuple_type->natts - 1 &&
			tuple_type->attrs[attnum - 1] != NULL &&
		  variable->vartype == tuple_type->attrs[attnum - 1]->atttypid));

	/*
	 * If the attribute number is invalid, then we are supposed to return
	 * the entire tuple, we give back a whole slot so that callers know
	 * what the tuple looks like.
	 */
	if (attnum == InvalidAttrNumber)
	{
		TupleTableSlot *tempSlot;
		TupleDesc	td;
		HeapTuple	tup;

		tempSlot = makeNode(TupleTableSlot);
		tempSlot->ttc_shouldFree = false;
		tempSlot->ttc_descIsNew = true;
		tempSlot->ttc_tupleDescriptor = (TupleDesc) NULL;
		tempSlot->ttc_buffer = InvalidBuffer;

		tup = heap_copytuple(heapTuple);
		td = CreateTupleDescCopy(tuple_type);

		ExecSetSlotDescriptor(tempSlot, td);

		ExecStoreTuple(tup, tempSlot, InvalidBuffer, true);
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
 *		so our job is to replace the param node with the datum
 *		containing the appropriate information ("sam").
 *
 *		Q: if we have a parameter ($.foo) without a binding, i.e.
 *		   there is no (foo = xxx) in the parameter list info,
 *		   is this a fatal error or should this be a "not available"
 *		   (in which case we shoud return a Const node with the
 *			isnull flag) ?	-cim 10/13/89
 *
 *		Minor modification: Param nodes now have an extra field,
 *		`paramkind' which specifies the type of parameter
 *		(see params.h). So while searching the paramList for
 *		a paramname/value pair, we have also to check for `kind'.
 *
 *		NOTE: The last entry in `paramList' is always an
 *		entry with kind == PARAM_INVALID.
 * ----------------------------------------------------------------
 */
Datum
ExecEvalParam(Param *expression, ExprContext *econtext, bool *isNull)
{
	char	   *thisParameterName;
	int			thisParameterKind = expression->paramkind;
	AttrNumber	thisParameterId = expression->paramid;
	int			matchFound;
	ParamListInfo paramList;

	if (thisParameterKind == PARAM_EXEC)
	{
		ParamExecData *prm;

		prm = &(econtext->ecxt_param_exec_vals[thisParameterId]);
		if (prm->execPlan != NULL)
		{
			ExecSetParamPlan(prm->execPlan, econtext);
			/* ExecSetParamPlan should have processed this param... */
			Assert(prm->execPlan == NULL);
		}
		*isNull = prm->isnull;
		return prm->value;
	}

	thisParameterName = expression->paramname;
	paramList = econtext->ecxt_param_list_info;

	*isNull = false;

	/*
	 * search the list with the parameter info to find a matching name. An
	 * entry with an InvalidName denotes the last element in the array.
	 */
	matchFound = 0;
	if (paramList != NULL)
	{

		/*
		 * search for an entry in 'paramList' that matches the
		 * `expression'.
		 */
		while (paramList->kind != PARAM_INVALID && !matchFound)
		{
			switch (thisParameterKind)
			{
				case PARAM_NAMED:
					if (thisParameterKind == paramList->kind &&
						strcmp(paramList->name, thisParameterName) == 0)
						matchFound = 1;
					break;
				case PARAM_NUM:
					if (thisParameterKind == paramList->kind &&
						paramList->id == thisParameterId)
						matchFound = 1;
					break;
				case PARAM_OLD:
				case PARAM_NEW:
					if (thisParameterKind == paramList->kind &&
						paramList->id == thisParameterId)
					{
						matchFound = 1;

						/*
						 * sanity check
						 */
						if (strcmp(paramList->name, thisParameterName) != 0)
						{
							elog(ERROR,
								 "ExecEvalParam: new/old params with same id & diff names");
						}
					}
					break;
				default:

					/*
					 * oops! this is not supposed to happen!
					 */
					elog(ERROR, "ExecEvalParam: invalid paramkind %d",
						 thisParameterKind);
			}
			if (!matchFound)
				paramList++;
		}						/* while */
	}							/* if */

	if (!matchFound)
	{

		/*
		 * ooops! we couldn't find this parameter in the parameter list.
		 * Signal an error
		 */
		elog(ERROR, "ExecEvalParam: Unknown value for parameter %s",
			 thisParameterName);
	}

	/*
	 * return the value.
	 */
	*isNull = paramList->isnull;
	return paramList->value;
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
		elog(ERROR, "GetAttributeByNum: Invalid attribute number");

	if (!AttrNumberIsForUserDefinedAttr(attrno))
		elog(ERROR, "GetAttributeByNum: cannot access system attributes here");

	if (isNull == (bool *) NULL)
		elog(ERROR, "GetAttributeByNum: a NULL isNull flag was passed");

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
		elog(ERROR, "GetAttributeByName: Invalid attribute name");

	if (isNull == (bool *) NULL)
		elog(ERROR, "GetAttributeByName: a NULL isNull flag was passed");

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
		elog(ERROR, "GetAttributeByName: attribute %s not found", attname);

	retval = heap_getattr(slot->val,
						  attrno,
						  tupdesc,
						  isNull);
	if (*isNull)
		return (Datum) 0;

	return retval;
}

/*
 * Evaluate arguments for a function.
 */
static ExprDoneCond
ExecEvalFuncArgs(FunctionCachePtr fcache,
				 List *argList,
				 ExprContext *econtext)
{
	ExprDoneCond argIsDone;
	int			i;
	List	   *arg;

	argIsDone = ExprSingleResult; /* default assumption */

	i = 0;
	foreach(arg, argList)
	{
		ExprDoneCond	thisArgIsDone;

		fcache->fcinfo.arg[i] = ExecEvalExpr((Node *) lfirst(arg),
											 econtext,
											 &fcache->fcinfo.argnull[i],
											 &thisArgIsDone);

		if (thisArgIsDone != ExprSingleResult)
		{
			/*
			 * We allow only one argument to have a set value; we'd need
			 * much more complexity to keep track of multiple set arguments
			 * (cf. ExecTargetList) and it doesn't seem worth it.
			 */
			if (argIsDone != ExprSingleResult)
				elog(ERROR, "Functions and operators can take only one set argument");
			fcache->hasSetArg = true;
			argIsDone = thisArgIsDone;
		}
		i++;
	}

	return argIsDone;
}

/*
 *		ExecMakeFunctionResult
 *
 * Evaluate the arguments to a function and then the function itself.
 *
 * NOTE: econtext is used only for evaluating the argument expressions;
 * it is not passed to the function itself.
 */
Datum
ExecMakeFunctionResult(FunctionCachePtr fcache,
					   List *arguments,
					   ExprContext *econtext,
					   bool *isNull,
					   ExprDoneCond *isDone)
{
	Datum				result;
	ExprDoneCond		argDone;
	int					i;

	/*
	 * arguments is a list of expressions to evaluate before passing to
	 * the function manager.  We skip the evaluation if it was already
	 * done in the previous call (ie, we are continuing the evaluation
	 * of a set-valued function).  Otherwise, collect the current argument
	 * values into fcache->fcinfo.
	 */
	if (fcache->fcinfo.nargs > 0 && !fcache->argsValid)
	{
		argDone = ExecEvalFuncArgs(fcache, arguments, econtext);
		if (argDone == ExprEndResult)
		{
			/* input is an empty set, so return an empty set. */
			*isNull = true;
			if (isDone)
				*isDone = ExprEndResult;
			else
				elog(ERROR, "Set-valued function called in context that cannot accept a set");
			return (Datum) 0;
		}
	}

	/*
	 * now return the value gotten by calling the function manager,
	 * passing the function the evaluated parameter values.
	 */
	if (fcache->func.fn_retset || fcache->hasSetArg)
	{
		/*
		 * We need to return a set result.  Complain if caller not ready
		 * to accept one.
		 */
		if (isDone == NULL)
			elog(ERROR, "Set-valued function called in context that cannot accept a set");

		/*
		 * This loop handles the situation where we have both a set argument
		 * and a set-valued function.  Once we have exhausted the function's
		 * value(s) for a particular argument value, we have to get the next
		 * argument value and start the function over again.  We might have
		 * to do it more than once, if the function produces an empty result
		 * set for a particular input value.
		 */
		for (;;)
		{
			/*
			 * If function is strict, and there are any NULL arguments,
			 * skip calling the function (at least for this set of args).
			 */
			bool	callit = true;

			if (fcache->func.fn_strict)
			{
				for (i = 0; i < fcache->fcinfo.nargs; i++)
				{
					if (fcache->fcinfo.argnull[i])
					{
						callit = false;
						break;
					}
				}
			}

			if (callit)
			{
				fcache->fcinfo.isnull = false;
				fcache->rsinfo.isDone = ExprSingleResult;
				result = FunctionCallInvoke(&fcache->fcinfo);
				*isNull = fcache->fcinfo.isnull;
				*isDone = fcache->rsinfo.isDone;
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
				 * Got a result from current argument.  If function itself
				 * returns set, flag that we want to reuse current argument
				 * values on next call.
				 */
				if (fcache->func.fn_retset)
					fcache->argsValid = true;
				/*
				 * Make sure we say we are returning a set, even if the
				 * function itself doesn't return sets.
				 */
				*isDone = ExprMultipleResult;
				break;
			}

			/* Else, done with this argument */
			fcache->argsValid = false;

			if (!fcache->hasSetArg)
				break;			/* input not a set, so done */

			/* Re-eval args to get the next element of the input set */
			argDone = ExecEvalFuncArgs(fcache, arguments, econtext);

			if (argDone != ExprMultipleResult)
			{

				/*
				 * End of arguments, so reset the hasSetArg flag and say
				 * "Done"
				 */
				fcache->hasSetArg = false;
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
		 * If function is strict, and there are any NULL arguments,
		 * skip calling the function and return NULL.
		 */
		if (fcache->func.fn_strict)
		{
			for (i = 0; i < fcache->fcinfo.nargs; i++)
			{
				if (fcache->fcinfo.argnull[i])
				{
					*isNull = true;
					return (Datum) 0;
				}
			}
		}
		fcache->fcinfo.isnull = false;
		result = FunctionCallInvoke(&fcache->fcinfo);
		*isNull = fcache->fcinfo.isnull;
	}

	return result;
}


/* ----------------------------------------------------------------
 *		ExecEvalOper
 *		ExecEvalFunc
 *
 *		Evaluate the functional result of a list of arguments by calling the
 *		function manager.
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecEvalOper
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalOper(Expr *opClause,
			 ExprContext *econtext,
			 bool *isNull,
			 ExprDoneCond *isDone)
{
	Oper	   *op;
	List	   *argList;
	FunctionCachePtr fcache;

	/*
	 * we extract the oid of the function associated with the op and then
	 * pass the work onto ExecMakeFunctionResult which evaluates the
	 * arguments and returns the result of calling the function on the
	 * evaluated arguments.
	 */
	op = (Oper *) opClause->oper;
	argList = opClause->args;

	/*
	 * get the fcache from the Oper node. If it is NULL, then initialize
	 * it
	 */
	fcache = op->op_fcache;
	if (fcache == NULL)
	{
		fcache = init_fcache(op->opid, length(argList),
							 econtext->ecxt_per_query_memory);
		op->op_fcache = fcache;
	}

	return ExecMakeFunctionResult(fcache, argList, econtext,
								  isNull, isDone);
}

/* ----------------------------------------------------------------
 *		ExecEvalFunc
 * ----------------------------------------------------------------
 */

static Datum
ExecEvalFunc(Expr *funcClause,
			 ExprContext *econtext,
			 bool *isNull,
			 ExprDoneCond *isDone)
{
	Func	   *func;
	List	   *argList;
	FunctionCachePtr fcache;

	/*
	 * we extract the oid of the function associated with the func node and
	 * then pass the work onto ExecMakeFunctionResult which evaluates the
	 * arguments and returns the result of calling the function on the
	 * evaluated arguments.
	 *
	 * this is nearly identical to the ExecEvalOper code.
	 */
	func = (Func *) funcClause->oper;
	argList = funcClause->args;

	/*
	 * get the fcache from the Func node. If it is NULL, then initialize
	 * it
	 */
	fcache = func->func_fcache;
	if (fcache == NULL)
	{
		fcache = init_fcache(func->funcid, length(argList),
							 econtext->ecxt_per_query_memory);
		func->func_fcache = fcache;
	}

	return ExecMakeFunctionResult(fcache, argList, econtext,
								  isNull, isDone);
}

/* ----------------------------------------------------------------
 *		ExecEvalNot
 *		ExecEvalOr
 *		ExecEvalAnd
 *
 *		Evaluate boolean expressions.  Evaluation of 'or' is
 *		short-circuited when the first true (or null) value is found.
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
ExecEvalNot(Expr *notclause, ExprContext *econtext, bool *isNull)
{
	Node	   *clause;
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
	return BoolGetDatum(! DatumGetBool(expr_value));
}

/* ----------------------------------------------------------------
 *		ExecEvalOr
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalOr(Expr *orExpr, ExprContext *econtext, bool *isNull)
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
		clause_value = ExecEvalExpr((Node *) lfirst(clause),
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
ExecEvalAnd(Expr *andExpr, ExprContext *econtext, bool *isNull)
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
		clause_value = ExecEvalExpr((Node *) lfirst(clause),
									econtext, isNull, NULL);

		/*
		 * if we have a non-null false result, then return it.
		 */
		if (*isNull)
			AnyNull = true;		/* remember we got a null */
		else if (! DatumGetBool(clause_value))
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
ExecEvalCase(CaseExpr *caseExpr, ExprContext *econtext,
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
		CaseWhen   *wclause = lfirst(clause);

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
 *		ExecEvalFieldSelect
 *
 *		Evaluate a FieldSelect node.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalFieldSelect(FieldSelect *fselect,
					ExprContext *econtext,
					bool *isNull,
					ExprDoneCond *isDone)
{
	Datum			result;
	TupleTableSlot *resSlot;

	result = ExecEvalExpr(fselect->arg, econtext, isNull, isDone);
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
 *		expression: the expression tree to evaluate
 *		econtext: evaluation context information
 *
 * Outputs:
 *		return value: Datum value of result
 *		*isNull: set to TRUE if result is NULL (actual return value is
 *				 meaningless if so); set to FALSE if non-null result
 *		*isDone: set to indicator of set-result status
 *
 * A caller that can only accept a singleton (non-set) result should pass
 * NULL for isDone; if the expression computes a set result then an elog()
 * error will be reported.  If the caller does pass an isDone pointer then
 * *isDone is set to one of these three states:
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
 * do the switch in an outer loop.  We do not do the switch here because
 * it'd be a waste of cycles during recursive entries to ExecEvalExpr().
 *
 * This routine is an inner loop routine and must be as fast as possible.
 * ----------------------------------------------------------------
 */
Datum
ExecEvalExpr(Node *expression,
			 ExprContext *econtext,
			 bool *isNull,
			 ExprDoneCond *isDone)
{
	Datum		retDatum;

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
	switch (nodeTag(expression))
	{
		case T_Var:
			retDatum = ExecEvalVar((Var *) expression, econtext, isNull);
			break;
		case T_Const:
			{
				Const	   *con = (Const *) expression;

				retDatum = con->constvalue;
				*isNull = con->constisnull;
				break;
			}
		case T_Param:
			retDatum = ExecEvalParam((Param *) expression, econtext, isNull);
			break;
		case T_Iter:
			retDatum = ExecEvalIter((Iter *) expression,
									econtext,
									isNull,
									isDone);
			break;
		case T_Aggref:
			retDatum = ExecEvalAggref((Aggref *) expression, econtext, isNull);
			break;
		case T_ArrayRef:
			retDatum = ExecEvalArrayRef((ArrayRef *) expression,
										econtext,
										isNull,
										isDone);
			break;
		case T_Expr:
			{
				Expr	   *expr = (Expr *) expression;

				switch (expr->opType)
				{
					case OP_EXPR:
						retDatum = ExecEvalOper(expr, econtext,
												isNull, isDone);
						break;
					case FUNC_EXPR:
						retDatum = ExecEvalFunc(expr, econtext,
												isNull, isDone);
						break;
					case OR_EXPR:
						retDatum = ExecEvalOr(expr, econtext, isNull);
						break;
					case AND_EXPR:
						retDatum = ExecEvalAnd(expr, econtext, isNull);
						break;
					case NOT_EXPR:
						retDatum = ExecEvalNot(expr, econtext, isNull);
						break;
					case SUBPLAN_EXPR:
						retDatum = ExecSubPlan((SubPlan *) expr->oper,
											   expr->args, econtext,
											   isNull);
						break;
					default:
						elog(ERROR, "ExecEvalExpr: unknown expression type %d",
							 expr->opType);
						retDatum = 0;	/* keep compiler quiet */
						break;
				}
				break;
			}
		case T_FieldSelect:
			retDatum = ExecEvalFieldSelect((FieldSelect *) expression,
										   econtext,
										   isNull,
										   isDone);
			break;
		case T_RelabelType:
			retDatum = ExecEvalExpr(((RelabelType *) expression)->arg,
									econtext,
									isNull,
									isDone);
			break;
		case T_CaseExpr:
			retDatum = ExecEvalCase((CaseExpr *) expression,
									econtext,
									isNull,
									isDone);
			break;

		default:
			elog(ERROR, "ExecEvalExpr: unknown expression type %d",
				 nodeTag(expression));
			retDatum = 0;		/* keep compiler quiet */
			break;
	}

	return retDatum;
}	/* ExecEvalExpr() */


/*
 * Same as above, but get into the right allocation context explicitly.
 */
Datum
ExecEvalExprSwitchContext(Node *expression,
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
		Node	   *clause = (Node *) lfirst(qlist);
		Datum		expr_value;
		bool		isNull;

		expr_value = ExecEvalExpr(clause, econtext, &isNull, NULL);

		if (isNull)
		{
			if (resultForNull == false)
			{
				result = false;	/* treat NULL as FALSE */
				break;
			}
		}
		else
		{
			if (! DatumGetBool(expr_value))
			{
				result = false;	/* definitely FALSE */
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
	int			len = 0;
	List	   *tl;

	foreach(tl, targetlist)
	{
		TargetEntry	   *curTle = (TargetEntry *) lfirst(tl);

		if (curTle->resdom != NULL)
			len++;
		else
			len += curTle->fjoin->fj_nNodes;
	}
	return len;
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
		TargetEntry	   *curTle = (TargetEntry *) lfirst(tl);

		if (curTle->resdom != NULL)
		{
			if (! curTle->resdom->resjunk)
				len++;
		}
		else
		{
			len += curTle->fjoin->fj_nNodes;
		}
	}
	return len;
}

/* ----------------------------------------------------------------
 *		ExecTargetList
 *
 *		Evaluates a targetlist with respect to the current
 *		expression context and return a tuple.
 *
 * As with ExecEvalExpr, the caller should pass isDone = NULL if not
 * prepared to deal with sets of result tuples.  Otherwise, a return
 * of *isDone = ExprMultipleResult signifies a set element, and a return
 * of *isDone = ExprEndResult signifies end of the set of tuple.
 * ----------------------------------------------------------------
 */
static HeapTuple
ExecTargetList(List *targetlist,
			   int nodomains,
			   TupleDesc targettype,
			   Datum *values,
			   ExprContext *econtext,
			   ExprDoneCond *isDone)
{
	MemoryContext oldContext;
#define NPREALLOCDOMAINS 64
	char		nullsArray[NPREALLOCDOMAINS];
	bool		fjIsNullArray[NPREALLOCDOMAINS];
	ExprDoneCond itemIsDoneArray[NPREALLOCDOMAINS];
	char	   *nulls;
	bool	   *fjIsNull;
	ExprDoneCond *itemIsDone;
	List	   *tl;
	TargetEntry *tle;
	AttrNumber	resind;
	HeapTuple	newTuple;
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
	 * allocate an array of char's to hold the "null" information only if
	 * we have a really large targetlist.  otherwise we use the stack.
	 *
	 * We also allocate a bool array that is used to hold fjoin result state,
	 * and another array that holds the isDone status for each targetlist item.
	 * The isDone status is needed so that we can iterate, generating multiple
	 * tuples, when one or more tlist items return sets.  (We expect the caller
	 * to call us again if we return *isDone = ExprMultipleResult.)
	 */
	if (nodomains > NPREALLOCDOMAINS)
	{
		nulls = (char *) palloc(nodomains * sizeof(char));
		fjIsNull = (bool *) palloc(nodomains * sizeof(bool));
		itemIsDone = (ExprDoneCond *) palloc(nodomains * sizeof(ExprDoneCond));
	}
	else
	{
		nulls = nullsArray;
		fjIsNull = fjIsNullArray;
		itemIsDone = itemIsDoneArray;
	}

	/*
	 * evaluate all the expressions in the target list
	 */

	if (isDone)
		*isDone = ExprSingleResult;	/* until proven otherwise */

	haveDoneSets = false;		/* any exhausted set exprs in tlist? */

	foreach(tl, targetlist)
	{
		tle = lfirst(tl);

		if (tle->resdom != NULL)
		{
			resind = tle->resdom->resno - 1;

			values[resind] = ExecEvalExpr(tle->expr,
										  econtext,
										  &isNull,
										  &itemIsDone[resind]);
			nulls[resind] = isNull ? 'n' : ' ';

			if (itemIsDone[resind] != ExprSingleResult)
			{
				/* We have a set-valued expression in the tlist */
				if (isDone == NULL)
					elog(ERROR, "Set-valued function called in context that cannot accept a set");
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
		else
		{
#ifdef SETS_FIXED
			int			curNode;
			Resdom	   *fjRes;
			List	   *fjTlist = (List *) tle->expr;
			Fjoin	   *fjNode = tle->fjoin;
			int			nNodes = fjNode->fj_nNodes;
			DatumPtr	results = fjNode->fj_results;

			ExecEvalFjoin(tle, econtext, fjIsNull, isDone);

			/* XXX this is wrong, but since fjoin code is completely broken
			 * anyway, I'm not going to worry about it now --- tgl 8/23/00
			 */
			if (isDone && *isDone == ExprEndResult)
			{
				MemoryContextSwitchTo(oldContext);
				newTuple = NULL;
				goto exit;
			}

			/*
			 * get the result from the inner node
			 */
			fjRes = (Resdom *) fjNode->fj_innerNode;
			resind = fjRes->resno - 1;
			values[resind] = results[0];
			nulls[resind] = fjIsNull[0] ? 'n' : ' ';

			/*
			 * Get results from all of the outer nodes
			 */
			for (curNode = 1;
				 curNode < nNodes;
				 curNode++, fjTlist = lnext(fjTlist))
			{
				Node	   *outernode = lfirst(fjTlist);

				fjRes = (Resdom *) outernode->iterexpr;
				resind = fjRes->resno - 1;
				values[resind] = results[curNode];
				nulls[resind] = fjIsNull[curNode] ? 'n' : ' ';
			}
#else
			elog(ERROR, "ExecTargetList: fjoin nodes not currently supported");
#endif
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
			 * all sets are done, so report that tlist expansion is complete.
			 */
			*isDone = ExprEndResult;
			MemoryContextSwitchTo(oldContext);
			newTuple = NULL;
			goto exit;
		}
		else
		{

			/*
			 * We have some done and some undone sets.  Restart the done
			 * ones so that we can deliver a tuple (if possible).
			 */
			foreach(tl, targetlist)
			{
				tle = lfirst(tl);

				if (tle->resdom != NULL)
				{
					resind = tle->resdom->resno - 1;

					if (itemIsDone[resind] == ExprEndResult)
					{
						values[resind] = ExecEvalExpr(tle->expr,
													  econtext,
													  &isNull,
													  &itemIsDone[resind]);
						nulls[resind] = isNull ? 'n' : ' ';

						if (itemIsDone[resind] == ExprEndResult)
						{

							/*
							 * Oh dear, this item is returning an empty
							 * set. Guess we can't make a tuple after all.
							 */
							*isDone = ExprEndResult;
							break;
						}
					}
				}
			}
			/*
			 * If we cannot make a tuple because some sets are empty,
			 * we still have to cycle the nonempty sets to completion,
			 * else resources will not be released from subplans etc.
			 */
			if (*isDone == ExprEndResult)
			{
				foreach(tl, targetlist)
				{
					tle = lfirst(tl);

					if (tle->resdom != NULL)
					{
						resind = tle->resdom->resno - 1;

						while (itemIsDone[resind] == ExprMultipleResult)
						{
							(void) ExecEvalExpr(tle->expr,
												econtext,
												&isNull,
												&itemIsDone[resind]);
						}
					}
				}

				MemoryContextSwitchTo(oldContext);
				newTuple = NULL;
				goto exit;
			}
		}
	}

	/*
	 * form the new result tuple (in the caller's memory context!)
	 */
	MemoryContextSwitchTo(oldContext);

	newTuple = (HeapTuple) heap_formtuple(targettype, values, nulls);

exit:

	/*
	 * free the status arrays if we palloc'd them
	 */
	if (nodomains > NPREALLOCDOMAINS)
	{
		pfree(nulls);
		pfree(fjIsNull);
		pfree(itemIsDone);
	}

	return newTuple;
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
	List	   *targetlist;
	int			len;
	TupleDesc	tupType;
	Datum	   *tupValue;
	ExprContext *econtext;
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
	targetlist = projInfo->pi_targetlist;
	len = projInfo->pi_len;
	tupType = slot->ttc_tupleDescriptor;

	tupValue = projInfo->pi_tupValue;
	econtext = projInfo->pi_exprContext;

	/*
	 * form a new result tuple (if possible --- result can be NULL)
	 */
	newTuple = ExecTargetList(targetlist,
							  len,
							  tupType,
							  tupValue,
							  econtext,
							  isDone);

	/*
	 * store the tuple in the projection slot and return the slot.
	 */
	return ExecStoreTuple(newTuple,			/* tuple to store */
						  slot,				/* slot to store in */
						  InvalidBuffer,	/* tuple has no buffer */
						  true);
}
