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
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execQual.c,v 1.76 2000/07/23 01:35:58 tgl Exp $
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
#include "catalog/pg_language.h"
#include "executor/execFlatten.h"
#include "executor/execdebug.h"
#include "executor/functions.h"
#include "executor/nodeSubplan.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fcache2.h"


/* static function decls */
static Datum ExecEvalAggref(Aggref *aggref, ExprContext *econtext, bool *isNull);
static Datum ExecEvalArrayRef(ArrayRef *arrayRef, ExprContext *econtext,
				 bool *isNull, bool *isDone);
static Datum ExecEvalOper(Expr *opClause, ExprContext *econtext,
			 bool *isNull);
static Datum ExecEvalFunc(Expr *funcClause, ExprContext *econtext,
			 bool *isNull, bool *isDone);
static void ExecEvalFuncArgs(FunctionCachePtr fcache, ExprContext *econtext,
							 List *argList, FunctionCallInfo fcinfo,
							 bool *argIsDone);
static Datum ExecEvalNot(Expr *notclause, ExprContext *econtext, bool *isNull);
static Datum ExecEvalAnd(Expr *andExpr, ExprContext *econtext, bool *isNull);
static Datum ExecEvalOr(Expr *orExpr, ExprContext *econtext, bool *isNull);
static Datum ExecEvalVar(Var *variable, ExprContext *econtext, bool *isNull);
static Datum ExecMakeFunctionResult(Node *node, List *arguments,
					   ExprContext *econtext, bool *isNull, bool *isDone);

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
				 bool *isDone)
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
	bool		dummy;

	*isNull = false;

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
													 &dummy));
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
														 &dummy));
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
											  &dummy);
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
	bool		byval;
	int16		len;

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
		tempSlot->ttc_whichplan = -1;

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

	/*
	 * return null if att is null
	 */
	if (*isNull)
		return (Datum) 0;

	/*
	 * get length and type information.. ??? what should we do about
	 * variable length attributes - variable length attributes have their
	 * length stored in the first 4 bytes of the memory pointed to by the
	 * returned value.. If we can determine that the type is a variable
	 * length type, we can do the right thing. -cim 9/15/89
	 */
	if (attnum < 0)
	{

		/*
		 * If this is a pseudo-att, we get the type and fake the length.
		 * There ought to be a routine to return the real lengths, so
		 * we'll mark this one ... XXX -mao
		 */
		len = heap_sysattrlen(attnum);	/* XXX see -mao above */
		byval = heap_sysattrbyval(attnum);		/* XXX see -mao above */
	}
	else
	{
		len = tuple_type->attrs[attnum - 1]->attlen;
		byval = tuple_type->attrs[attnum - 1]->attbyval ? true : false;
	}

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
	if (paramList->isnull)
	{
		*isNull = true;
		return (Datum) 0;
	}

	if (expression->param_tlist != NIL)
	{
		HeapTuple	tup;
		Datum		value;
		List	   *tlist = expression->param_tlist;
		TargetEntry *tle = (TargetEntry *) lfirst(tlist);
		TupleTableSlot *slot = (TupleTableSlot *) paramList->value;

		tup = slot->val;
		value = ProjectAttribute(slot->ttc_tupleDescriptor,
								 tle, tup, isNull);
		return value;
	}
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
 *
 * XXX these two functions are misdeclared: they should be declared to
 * return Datum.  They are not used anywhere in the backend proper, and
 * exist only for use by user-defined functions.  Should we change their
 * definitions, at risk of breaking user code?
 */
char *
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
		return (char *) NULL;
	}

	retval = heap_getattr(slot->val,
						  attrno,
						  slot->ttc_tupleDescriptor,
						  isNull);
	if (*isNull)
		return (char *) NULL;
	return (char *) retval;
}

char *
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
		return (char *) NULL;
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
		return (char *) NULL;
	return (char *) retval;
}


static void
ExecEvalFuncArgs(FunctionCachePtr fcache,
				 ExprContext *econtext,
				 List *argList,
				 FunctionCallInfo fcinfo,
				 bool *argIsDone)
{
	int			i;
	List	   *arg;

	i = 0;
	foreach(arg, argList)
	{

		/*
		 * evaluate the expression, in general functions cannot take sets
		 * as arguments but we make an exception in the case of nested dot
		 * expressions.  We have to watch out for this case here.
		 */
		fcinfo->arg[i] = ExecEvalExpr((Node *) lfirst(arg),
									  econtext,
									  &fcinfo->argnull[i],
									  argIsDone);

		if (!(*argIsDone))
		{
			if (i != 0)
				elog(ERROR, "functions can only take sets in their first argument");
			fcache->setArg = fcinfo->arg[0];
			fcache->hasSetArg = true;
		}
		i++;
	}
}

/*
 *		ExecMakeFunctionResult
 */
static Datum
ExecMakeFunctionResult(Node *node,
					   List *arguments,
					   ExprContext *econtext,
					   bool *isNull,
					   bool *isDone)
{
	FunctionCallInfoData	fcinfo;
	FunctionCachePtr		fcache;
	List				   *ftlist;
	bool					funcisset;
	Datum					result;
	bool					argDone;

	MemSet(&fcinfo, 0, sizeof(fcinfo));

	/*
	 * This is kind of ugly, Func nodes now have targetlists so that we
	 * know when and what to project out from postquel function results.
	 * ExecMakeFunctionResult becomes a little bit more of a dual personality
	 * as a result.
	 */
	if (IsA(node, Func))
	{
		fcache = ((Func *) node)->func_fcache;
		ftlist = ((Func *) node)->func_tlist;
		funcisset = (((Func *) node)->funcid == F_SETEVAL);
	}
	else
	{
		fcache = ((Oper *) node)->op_fcache;
		ftlist = NIL;
		funcisset = false;
	}

	fcinfo.flinfo = &fcache->func;
	fcinfo.nargs = fcache->nargs;

	/*
	 * arguments is a list of expressions to evaluate before passing to
	 * the function manager.  We collect the results of evaluating the
	 * expressions into the FunctionCallInfo struct.  Note we assume that
	 * fcache->nargs is the correct length of the arguments list!
	 */
	if (fcache->nargs > 0)
	{
		if (fcache->nargs > FUNC_MAX_ARGS)
			elog(ERROR, "ExecMakeFunctionResult: too many arguments");

		/*
		 * If the setArg in the fcache is set we have an argument
		 * returning a set of tuples (i.e. a nested dot expression).  We
		 * don't want to evaluate the arguments again until the function
		 * is done. hasSetArg will always be false until we eval the args
		 * for the first time.
		 */
		if (fcache->hasSetArg && fcache->setArg != (Datum) 0)
		{
			fcinfo.arg[0] = fcache->setArg;
			argDone = false;
		}
		else
			ExecEvalFuncArgs(fcache, econtext, arguments, &fcinfo, &argDone);

		if (fcache->hasSetArg && argDone)
		{
			/* can only get here if input is an empty set. */
			*isNull = true;
			*isDone = true;
			return (Datum) 0;
		}
	}

	/*
	 * If this function is really a set, we have to diddle with things. If
	 * the function has already been called at least once, then the setArg
	 * field of the fcache holds the OID of this set in pg_proc.  (This is
	 * not quite legit, since the setArg field is really for functions
	 * which take sets of tuples as input - set functions take no inputs
	 * at all.	But it's a nice place to stash this value, for now.)
	 *
	 * If this is the first call of the set's function, then the call to
	 * ExecEvalFuncArgs above just returned the OID of the pg_proc tuple
	 * which defines this set.	So replace the existing funcid in the
	 * funcnode with the set's OID.  Also, we want a new fcache which
	 * points to the right function, so get that, now that we have the
	 * right OID.  Also zero out fcinfo.arg, since the real set doesn't take
	 * any arguments.
	 */
	if (funcisset)
	{
		if (fcache->setArg)
		{
			((Func *) node)->funcid = DatumGetObjectId(fcache->setArg);
		}
		else
		{
			((Func *) node)->funcid = DatumGetObjectId(fcinfo.arg[0]);
			setFcache(node, DatumGetObjectId(fcinfo.arg[0]), NIL, econtext);
			fcache = ((Func *) node)->func_fcache;
			fcache->setArg = fcinfo.arg[0];
		}
		fcinfo.arg[0] = (Datum) 0;
	}

	/*
	 * now return the value gotten by calling the function manager,
	 * passing the function the evaluated parameter values.
	 */
	if (fcache->language == SQLlanguageId)
	{
		/*--------------------
		 * This loop handles the situation where we are iterating through
		 * all results in a nested dot function (whose argument function
		 * returns a set of tuples) and the current function finally
		 * finishes.  We need to get the next argument in the set and start
		 * the function all over again.  We might have to do it more than
		 * once, if the function produces no results for a particular argument.
		 * This is getting unclean.
		 *--------------------
		 */
		for (;;)
		{
			/*
			 * If function is strict, and there are any NULL arguments,
			 * skip calling the function (at least for this set of args).
			 */
			bool	callit = true;

			if (fcinfo.flinfo->fn_strict)
			{
				int		i;

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
				result = postquel_function(&fcinfo, fcache, ftlist, isDone);
				*isNull = fcinfo.isnull;
			}
			else
			{
				result = (Datum) 0;
				*isNull = true;
				*isDone = true;
			}

			if (!*isDone)
				break;			/* got a result from current argument */
			if (!fcache->hasSetArg)
				break;			/* input not a set, so done */

			/* OK, get the next argument... */
			ExecEvalFuncArgs(fcache, econtext, arguments, &fcinfo, &argDone);

			if (argDone)
			{

				/*
				 * End of arguments, so reset the setArg flag and say
				 * "Done"
				 */
				fcache->setArg = (Datum) 0;
				fcache->hasSetArg = false;
				*isDone = true;
				*isNull = true;
				result = (Datum) 0;
				break;
			}

			/*
			 * If we reach here, loop around to run the function on the
			 * new argument.
			 */
		}

		if (funcisset)
		{

			/*
			 * reset the funcid so that next call to this routine will
			 * still recognize this func as a set. Note that for now we
			 * assume that the set function in pg_proc must be a Postquel
			 * function - the funcid is not reset below for C functions.
			 */
			((Func *) node)->funcid = F_SETEVAL;

			/*
			 * If we're done with the results of this function, get rid of
			 * its func cache.
			 */
			if (*isDone)
				((Func *) node)->func_fcache = NULL;
		}
	}
	else
	{
		/* A non-SQL function cannot return a set, at present. */
		*isDone = true;

		/*
		 * If function is strict, and there are any NULL arguments,
		 * skip calling the function and return NULL.
		 */
		if (fcinfo.flinfo->fn_strict)
		{
			int		i;

			for (i = 0; i < fcinfo.nargs; i++)
			{
				if (fcinfo.argnull[i])
				{
					*isNull = true;
					return (Datum) 0;
				}
			}
		}
		result = FunctionCallInvoke(&fcinfo);
		*isNull = fcinfo.isnull;
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
ExecEvalOper(Expr *opClause, ExprContext *econtext, bool *isNull)
{
	Oper	   *op;
	List	   *argList;
	FunctionCachePtr fcache;
	bool		isDone;

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
		setFcache((Node *) op, op->opid, argList, econtext);
		fcache = op->op_fcache;
	}

	/*
	 * call ExecMakeFunctionResult() with a dummy isDone that we ignore.
	 * We don't have operator whose arguments are sets.
	 */
	return ExecMakeFunctionResult((Node *) op, argList, econtext,
								  isNull, &isDone);
}

/* ----------------------------------------------------------------
 *		ExecEvalFunc
 * ----------------------------------------------------------------
 */

static Datum
ExecEvalFunc(Expr *funcClause,
			 ExprContext *econtext,
			 bool *isNull,
			 bool *isDone)
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
		setFcache((Node *) func, func->funcid, argList, econtext);
		fcache = func->func_fcache;
	}

	return ExecMakeFunctionResult((Node *) func, argList, econtext,
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
	bool		isDone;

	clause = lfirst(notclause->args);

	/*
	 * We don't iterate over sets in the quals, so pass in an isDone flag,
	 * but ignore it.
	 */
	expr_value = ExecEvalExpr(clause, econtext, isNull, &isDone);

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
	bool		isDone;
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

		/*
		 * We don't iterate over sets in the quals, so pass in an isDone
		 * flag, but ignore it.
		 */
		clause_value = ExecEvalExpr((Node *) lfirst(clause),
									econtext,
									isNull,
									&isDone);

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
	bool		isDone;
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

		/*
		 * We don't iterate over sets in the quals, so pass in an isDone
		 * flag, but ignore it.
		 */
		clause_value = ExecEvalExpr((Node *) lfirst(clause),
									econtext,
									isNull,
									&isDone);

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
ExecEvalCase(CaseExpr *caseExpr, ExprContext *econtext, bool *isNull)
{
	List	   *clauses;
	List	   *clause;
	Datum		clause_value;
	bool		isDone;

	clauses = caseExpr->args;

	/*
	 * we evaluate each of the WHEN clauses in turn, as soon as one is
	 * true we return the corresponding result. If none are true then we
	 * return the value of the default clause, or NULL if there is none.
	 */
	foreach(clause, clauses)
	{
		CaseWhen   *wclause = lfirst(clause);

		/*
		 * We don't iterate over sets in the quals, so pass in an isDone
		 * flag, but ignore it.
		 */
		clause_value = ExecEvalExpr(wclause->expr,
									econtext,
									isNull,
									&isDone);

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
								&isDone);
		}
	}

	if (caseExpr->defresult)
	{
		return ExecEvalExpr(caseExpr->defresult,
							econtext,
							isNull,
							&isDone);
	}

	*isNull = true;
	return (Datum) 0;
}

/* ----------------------------------------------------------------
 *		ExecEvalExpr
 *
 *		Recursively evaluate a targetlist or qualification expression.
 *
 *		The caller should already have switched into the temporary
 *		memory context econtext->ecxt_per_tuple_memory.  The convenience
 *		entry point ExecEvalExprSwitchContext() is provided for callers
 *		who don't prefer to do the switch in an outer loop.  We do not
 *		do the switch here because it'd be a waste of cycles during
 *		recursive entries to ExecEvalExpr().
 *
 *		This routine is an inner loop routine and must be as fast
 *		as possible.
 * ----------------------------------------------------------------
 */
Datum
ExecEvalExpr(Node *expression,
			 ExprContext *econtext,
			 bool *isNull,
			 bool *isDone)
{
	Datum		retDatum;

	/* Set default values for result flags: non-null, not a set result */
	*isNull = false;
	*isDone = true;

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
						retDatum = ExecEvalOper(expr, econtext, isNull);
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
		case T_RelabelType:
			retDatum = ExecEvalExpr(((RelabelType *) expression)->arg,
									econtext,
									isNull,
									isDone);
			break;
		case T_CaseExpr:
			retDatum = ExecEvalCase((CaseExpr *) expression, econtext, isNull);
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
						  bool *isDone)
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
		bool		isDone;

		/*
		 * pass isDone, but ignore it.	We don't iterate over multiple
		 * returns in the qualifications.
		 */
		expr_value = ExecEvalExpr(clause, econtext, &isNull, &isDone);

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

int
ExecTargetListLength(List *targetlist)
{
	int			len;
	List	   *tl;
	TargetEntry *curTle;

	len = 0;
	foreach(tl, targetlist)
	{
		curTle = lfirst(tl);

		if (curTle->resdom != NULL)
			len++;
		else
			len += curTle->fjoin->fj_nNodes;
	}
	return len;
}

/* ----------------------------------------------------------------
 *		ExecTargetList
 *
 *		Evaluates a targetlist with respect to the current
 *		expression context and return a tuple.
 * ----------------------------------------------------------------
 */
static HeapTuple
ExecTargetList(List *targetlist,
			   int nodomains,
			   TupleDesc targettype,
			   Datum *values,
			   ExprContext *econtext,
			   bool *isDone)
{
	MemoryContext oldContext;
	char		nulls_array[64];
	bool		fjNullArray[64];
	bool		itemIsDoneArray[64];
	char	   *null_head;
	bool	   *fjIsNull;
	bool	   *itemIsDone;
	List	   *tl;
	TargetEntry *tle;
	Node	   *expr;
	Resdom	   *resdom;
	AttrNumber	resind;
	Datum		constvalue;
	HeapTuple	newTuple;
	bool		isNull;
	bool		haveDoneIters;
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
	 * and another that holds the isDone status for each targetlist item.
	 */
	if (nodomains > 64)
	{
		null_head = (char *) palloc(nodomains + 1);
		fjIsNull = (bool *) palloc(nodomains + 1);
		itemIsDone = (bool *) palloc(nodomains + 1);
	}
	else
	{
		null_head = &nulls_array[0];
		fjIsNull = &fjNullArray[0];
		itemIsDone = &itemIsDoneArray[0];
	}

	/*
	 * evaluate all the expressions in the target list
	 */

	*isDone = true;				/* until proven otherwise */
	haveDoneIters = false;		/* any isDone Iter exprs in tlist? */

	foreach(tl, targetlist)
	{

		/*
		 * remember, a target list is a list of lists:
		 *
		 * ((<resdom | fjoin> expr) (<resdom | fjoin> expr) ...)
		 *
		 * tl is a pointer to successive cdr's of the targetlist tle is a
		 * pointer to the target list entry in tl
		 */
		tle = lfirst(tl);

		if (tle->resdom != NULL)
		{
			expr = tle->expr;
			resdom = tle->resdom;
			resind = resdom->resno - 1;

			constvalue = ExecEvalExpr(expr,
									  econtext,
									  &isNull,
									  &itemIsDone[resind]);

			values[resind] = constvalue;

			if (!isNull)
				null_head[resind] = ' ';
			else
				null_head[resind] = 'n';

			if (IsA(expr, Iter))
			{
				if (itemIsDone[resind])
					haveDoneIters = true;
				else
					*isDone = false;	/* we have undone Iters in the
										 * list */
			}
		}
		else
		{
			int			curNode;
			Resdom	   *fjRes;
			List	   *fjTlist = (List *) tle->expr;
			Fjoin	   *fjNode = tle->fjoin;
			int			nNodes = fjNode->fj_nNodes;
			DatumPtr	results = fjNode->fj_results;

			ExecEvalFjoin(tle, econtext, fjIsNull, isDone);

			/* this is probably wrong: */
			if (*isDone)
			{
				newTuple = NULL;
				goto exit;
			}

			/*
			 * get the result from the inner node
			 */
			fjRes = (Resdom *) fjNode->fj_innerNode;
			resind = fjRes->resno - 1;
			if (fjIsNull[0])
				null_head[resind] = 'n';
			else
			{
				null_head[resind] = ' ';
				values[resind] = results[0];
			}

			/*
			 * Get results from all of the outer nodes
			 */
			for (curNode = 1;
				 curNode < nNodes;
				 curNode++, fjTlist = lnext(fjTlist))
			{
#ifdef NOT_USED					/* what is this?? */
				Node	   *outernode = lfirst(fjTlist);

				fjRes = (Resdom *) outernode->iterexpr;
#endif
				resind = fjRes->resno - 1;
				if (fjIsNull[curNode])
					null_head[resind] = 'n';
				else
				{
					null_head[resind] = ' ';
					values[resind] = results[curNode];
				}
			}
		}
	}

	if (haveDoneIters)
	{
		if (*isDone)
		{

			/*
			 * all Iters are done, so return a null indicating tlist set
			 * expansion is complete.
			 */
			newTuple = NULL;
			goto exit;
		}
		else
		{

			/*
			 * We have some done and some undone Iters.  Restart the done
			 * ones so that we can deliver a tuple (if possible).
			 *
			 * XXX this code is a crock, because it only works for Iters at
			 * the top level of tlist expressions, and doesn't even work
			 * right for them: you should get all possible combinations of
			 * Iter results, but you won't unless the numbers of values
			 * returned by each are relatively prime.  Should have a
			 * mechanism more like aggregate functions, where we make a
			 * list of all Iters contained in the tlist and cycle through
			 * their values in a methodical fashion.  To do someday; can't
			 * get excited about fixing a Berkeley feature that's not in
			 * SQL92.  (The only reason we're doing this much is that we
			 * have to be sure all the Iters are run to completion, or
			 * their subplan executors will have unreleased resources,
			 * e.g. pinned buffers...)
			 */
			foreach(tl, targetlist)
			{
				tle = lfirst(tl);

				if (tle->resdom != NULL)
				{
					expr = tle->expr;
					resdom = tle->resdom;
					resind = resdom->resno - 1;

					if (IsA(expr, Iter) &&itemIsDone[resind])
					{
						constvalue = ExecEvalExpr(expr,
												  econtext,
												  &isNull,
												  &itemIsDone[resind]);
						if (itemIsDone[resind])
						{

							/*
							 * Oh dear, this Iter is returning an empty
							 * set. Guess we can't make a tuple after all.
							 */
							*isDone = true;
							newTuple = NULL;
							goto exit;
						}

						values[resind] = constvalue;

						if (!isNull)
							null_head[resind] = ' ';
						else
							null_head[resind] = 'n';
					}
				}
			}
		}
	}

	/*
	 * form the new result tuple (in the caller's memory context!)
	 */
	MemoryContextSwitchTo(oldContext);

	newTuple = (HeapTuple) heap_formtuple(targettype, values, null_head);

exit:

	/*
	 * free the status arrays if we palloc'd them
	 */
	if (nodomains > 64)
	{
		pfree(null_head);
		pfree(fjIsNull);
		pfree(itemIsDone);
	}

	/* make sure we are in the right context if we did "goto exit" */
	MemoryContextSwitchTo(oldContext);

	return newTuple;
}

/* ----------------------------------------------------------------
 *		ExecProject
 *
 *		projects a tuple based in projection info and stores
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
ExecProject(ProjectionInfo *projInfo, bool *isDone)
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
	 * form a new (result) tuple
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
	return (TupleTableSlot *)
		ExecStoreTuple(newTuple,/* tuple to store */
					   slot,	/* slot to store in */
					   InvalidBuffer,	/* tuple has no buffer */
					   true);
}
