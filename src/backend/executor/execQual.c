/*-------------------------------------------------------------------------
 *
 * execQual.c--
 *	  Routines to evaluate qualification and targetlist expressions
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execQual.c,v 1.27 1998/03/30 16:35:50 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		ExecEvalExpr	- evaluate an expression and return a datum
 *		ExecQual		- return true/false if qualification is satisified
 *		ExecTargetList	- form a new tuple by projecting the given tuple
 *
 *	 NOTES
 *		ExecEvalExpr() and ExecEvalVar() are hotspots.	making these faster
 *		will speed up the entire system.  Unfortunately they are currently
 *		implemented recursively..  Eliminating the recursion is bound to
 *		improve the speed of the executor.
 *
 *		ExecTargetList() is used to make tuple projections.  Rather then
 *		trying to speed it up, the execution plan should be pre-processed
 *		to facilitate attribute sharing between nodes wherever possible,
 *		instead of doing needless copying.	-cim 5/31/91
 *
 */
#include <string.h>

#include "postgres.h"
#include "fmgr.h"

#include "nodes/primnodes.h"
#include "nodes/relation.h"

#include "optimizer/clauses.h"

#include "nodes/memnodes.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "executor/executor.h"
#include "executor/execdebug.h"
#include "executor/execFlatten.h"
#include "executor/functions.h"
#include "executor/nodeSubplan.h"
#include "access/heapam.h"
#include "utils/memutils.h"
#include "utils/builtins.h"
#include "utils/palloc.h"
#include "utils/fcache.h"
#include "utils/fcache2.h"
#include "utils/array.h"
#include "utils/mcxt.h"

/* ----------------
 *		externs and constants
 * ----------------
 */

/*
 * XXX Used so we can get rid of use of Const nodes in the executor.
 * Currently only used by ExecHashGetBucket and set only by ExecMakeVarConst
 * and by ExecEvalArrayRef.
 */
bool		execConstByVal;
int			execConstLen;

/* static functions decls */
static Datum ExecEvalAggreg(Aggreg *agg, ExprContext *econtext, bool *isNull);
static Datum
ExecEvalArrayRef(ArrayRef *arrayRef, ExprContext *econtext,
				 bool *isNull, bool *isDone);
static Datum ExecEvalAnd(Expr *andExpr, ExprContext *econtext, bool *isNull);
static Datum
ExecEvalFunc(Expr *funcClause, ExprContext *econtext,
			 bool *isNull, bool *isDone);
static void
ExecEvalFuncArgs(FunctionCachePtr fcache, ExprContext *econtext,
				 List *argList, Datum argV[], bool *argIsDone);
static Datum ExecEvalNot(Expr *notclause, ExprContext *econtext, bool *isNull);
static Datum
ExecEvalOper(Expr *opClause, ExprContext *econtext,
			 bool *isNull);
static Datum ExecEvalOr(Expr *orExpr, ExprContext *econtext, bool *isNull);
static Datum ExecEvalVar(Var *variable, ExprContext *econtext, bool *isNull);
static Datum
ExecMakeFunctionResult(Node *node, List *arguments,
					   ExprContext *econtext, bool *isNull, bool *isDone);
static bool ExecQualClause(Node *clause, ExprContext *econtext);

/* --------------------------------
 *	  ExecEvalArrayRef
 *
 *	   This function takes an ArrayRef and returns a Const Node if it
 *	   is an array reference or returns the changed Array Node if it is
 *		   an array assignment.
 *
 * --------------------------------
 */
static Datum
ExecEvalArrayRef(ArrayRef *arrayRef,
				 ExprContext *econtext,
				 bool *isNull,
				 bool *isDone)
{
	bool		dummy;
	int			i = 0,
				j = 0;
	ArrayType  *array_scanner;
	List	   *upperIndexpr,
			   *lowerIndexpr;
	Node	   *assgnexpr;
	List	   *elt;
	IntArray	upper,
				lower;
	int		   *lIndex;
	char	   *dataPtr;

	*isNull = false;
	array_scanner = (ArrayType *) ExecEvalExpr(arrayRef->refexpr,
											   econtext,
											   isNull,
											   isDone);
	if (*isNull)
		return (Datum) NULL;

	upperIndexpr = arrayRef->refupperindexpr;

	foreach(elt, upperIndexpr)
	{
		upper.indx[i++] = (int32) ExecEvalExpr((Node *) lfirst(elt),
											   econtext,
											   isNull,
											   &dummy);
		if (*isNull)
			return (Datum) NULL;
	}

	lowerIndexpr = arrayRef->reflowerindexpr;
	lIndex = NULL;
	if (lowerIndexpr != NIL)
	{
		foreach(elt, lowerIndexpr)
		{
			lower.indx[j++] = (int32) ExecEvalExpr((Node *) lfirst(elt),
												   econtext,
												   isNull,
												   &dummy);
			if (*isNull)
				return (Datum) NULL;
		}
		if (i != j)
			elog(ERROR,
				 "ExecEvalArrayRef: upper and lower indices mismatch");
		lIndex = lower.indx;
	}

	assgnexpr = arrayRef->refassgnexpr;
	if (assgnexpr != NULL)
	{
		dataPtr = (char *) ExecEvalExpr((Node *)
										assgnexpr, econtext,
										isNull, &dummy);
		if (*isNull)
			return (Datum) NULL;
		execConstByVal = arrayRef->refelembyval;
		execConstLen = arrayRef->refelemlength;
		if (lIndex == NULL)
			return (Datum) array_set(array_scanner, i, upper.indx, dataPtr,
									 arrayRef->refelembyval,
									 arrayRef->refelemlength,
									 arrayRef->refattrlength, isNull);
		return (Datum) array_assgn(array_scanner, i, upper.indx,
								   lower.indx,
								   (ArrayType *) dataPtr,
								   arrayRef->refelembyval,
								   arrayRef->refelemlength, isNull);
	}
	execConstByVal = arrayRef->refelembyval;
	execConstLen = arrayRef->refelemlength;
	if (lIndex == NULL)
		return (Datum) array_ref(array_scanner, i, upper.indx,
								 arrayRef->refelembyval,
								 arrayRef->refelemlength,
								 arrayRef->refattrlength, isNull);
	return (Datum) array_clip(array_scanner, i, upper.indx, lower.indx,
							  arrayRef->refelembyval,
							  arrayRef->refelemlength, isNull);
}


/* ----------------------------------------------------------------
 *		ExecEvalAggreg
 *
 *		Returns a Datum whose value is the value of the precomputed
 *		aggregate found in the given expression context.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalAggreg(Aggreg *agg, ExprContext *econtext, bool *isNull)
{
	*isNull = econtext->ecxt_nulls[agg->aggno];	
	return econtext->ecxt_values[agg->aggno];
}

/* ----------------------------------------------------------------
 *		ExecEvalVar
 *
 *		Returns a Datum whose value is the value of a range
 *		variable with respect to given expression context.
 *
 *
 *		As an entry condition, we expect that the the datatype the
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
	Buffer		buffer;
	bool		byval;
	int16		len;

	/* ----------------
	 *	get the slot we want
	 * ----------------
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

	/* ----------------
	 *	 extract tuple information from the slot
	 * ----------------
	 */
	heapTuple = slot->val;
	tuple_type = slot->ttc_tupleDescriptor;
	buffer = slot->ttc_buffer;

	attnum = variable->varattno;

	/* (See prolog for explanation of this Assert) */
	Assert(attnum <= 0 ||
		   (attnum - 1 <= tuple_type->natts - 1 &&
			tuple_type->attrs[attnum - 1] != NULL &&
			variable->vartype == tuple_type->attrs[attnum - 1]->atttypid))

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
		tempSlot->ttc_tupleDescriptor = (TupleDesc) NULL,
			tempSlot->ttc_buffer = InvalidBuffer;
		tempSlot->ttc_whichplan = -1;

		tup = heap_copytuple(slot->val);
		td = CreateTupleDescCopy(slot->ttc_tupleDescriptor);

		ExecSetSlotDescriptor(tempSlot, td);

		ExecStoreTuple(tup, tempSlot, InvalidBuffer, true);
		return (Datum) tempSlot;
	}

	result = heap_getattr(heapTuple,	/* tuple containing attribute */
						  attnum,		/* attribute number of desired
										 * attribute */
						  tuple_type,	/* tuple descriptor of tuple */
						  isNull);		/* return: is attribute null? */

	/* ----------------
	 *	return null if att is null
	 * ----------------
	 */
	if (*isNull)
		return (Datum) NULL;

	/* ----------------
	 *	 get length and type information..
	 *	 ??? what should we do about variable length attributes
	 *	 - variable length attributes have their length stored
	 *	   in the first 4 bytes of the memory pointed to by the
	 *	   returned value.. If we can determine that the type
	 *	   is a variable length type, we can do the right thing.
	 *	   -cim 9/15/89
	 * ----------------
	 */
	if (attnum < 0)
	{
		/* ----------------
		 *	If this is a pseudo-att, we get the type and fake the length.
		 *	There ought to be a routine to return the real lengths, so
		 *	we'll mark this one ... XXX -mao
		 * ----------------
		 */
		len = heap_sysattrlen(attnum);	/* XXX see -mao above */
		byval = heap_sysattrbyval(attnum);		/* XXX see -mao above */
	}
	else
	{
		len = tuple_type->attrs[attnum - 1]->attlen;
		byval = tuple_type->attrs[attnum - 1]->attbyval ? true : false;
	}

	execConstByVal = byval;
	execConstLen = len;

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
		ParamExecData *prm = &(econtext->ecxt_param_exec_vals[thisParameterId]);

		if (prm->execPlan != NULL)
			ExecSetParamPlan(prm->execPlan);
		Assert(prm->execPlan == NULL);
		*isNull = prm->isnull;
		return (prm->value);
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
					{
						matchFound = 1;
					}
					break;
				case PARAM_NUM:
					if (thisParameterKind == paramList->kind &&
						paramList->id == thisParameterId)
					{
						matchFound = 1;
					}
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
			{
				paramList++;
			}
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
		return (Datum) NULL;
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
	return (paramList->value);
}


/* ----------------------------------------------------------------
 *		ExecEvalOper / ExecEvalFunc support routines
 * ----------------------------------------------------------------
 */

/* ----------------
 *		GetAttributeByName
 *		GetAttributeByNum
 *
 *		These are functions which return the value of the
 *		named attribute out of the tuple from the arg slot.  User defined
 *		C functions which take a tuple as an argument are expected
 *		to use this.  Ex: overpaid(EMP) might call GetAttributeByNum().
 * ----------------
 */
/* static but gets called from external functions */
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

/* XXX char16 name for catalogs */
#ifdef NOT_USED
char *
att_by_num(TupleTableSlot *slot,
		   AttrNumber attrno,
		   bool *isNull)
{
	return (GetAttributeByNum(slot, attrno, isNull));
}

#endif

char *
GetAttributeByName(TupleTableSlot *slot, char *attname, bool *isNull)
{
	AttrNumber	attrno;
	TupleDesc	tupdesc;
	HeapTuple	tuple;
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
	tuple = slot->val;

	natts = tuple->t_natts;

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

/* XXX char16 name for catalogs */
#ifdef NOT_USED
char *
att_by_name(TupleTableSlot *slot, char *attname, bool *isNull)
{
	return (GetAttributeByName(slot, attname, isNull));
}

#endif

static void
ExecEvalFuncArgs(FunctionCachePtr fcache,
				 ExprContext *econtext,
				 List *argList,
				 Datum argV[],
				 bool *argIsDone)
{
	int			i;
	bool		argIsNull,
			   *nullVect;
	List	   *arg;

	nullVect = fcache->nullVect;

	i = 0;
	foreach(arg, argList)
	{
		/* ----------------
		 *	 evaluate the expression, in general functions cannot take
		 *	 sets as arguments but we make an exception in the case of
		 *	 nested dot expressions.  We have to watch out for this case
		 *	 here.
		 * ----------------
		 */
		argV[i] = (Datum)
			ExecEvalExpr((Node *) lfirst(arg),
						 econtext,
						 &argIsNull,
						 argIsDone);


		if (!(*argIsDone))
		{
			Assert(i == 0);
			fcache->setArg = (char *) argV[0];
			fcache->hasSetArg = true;
		}
		if (argIsNull)
			nullVect[i] = true;
		else
			nullVect[i] = false;
		i++;
	}
}

/* ----------------
 *		ExecMakeFunctionResult
 * ----------------
 */
static Datum
ExecMakeFunctionResult(Node *node,
					   List *arguments,
					   ExprContext *econtext,
					   bool *isNull,
					   bool *isDone)
{
	Datum		argv[MAXFMGRARGS];
	FunctionCachePtr fcache;
	Func	   *funcNode = NULL;
	Oper	   *operNode = NULL;
	bool		funcisset = false;

	/*
	 * This is kind of ugly, Func nodes now have targetlists so that we
	 * know when and what to project out from postquel function results.
	 * This means we have to pass the func node all the way down instead
	 * of using only the fcache struct as before.  ExecMakeFunctionResult
	 * becomes a little bit more of a dual personality as a result.
	 */
	if (IsA(node, Func))
	{
		funcNode = (Func *) node;
		fcache = funcNode->func_fcache;
	}
	else
	{
		operNode = (Oper *) node;
		fcache = operNode->op_fcache;
	}

	/* ----------------
	 *	arguments is a list of expressions to evaluate
	 *	before passing to the function manager.
	 *	We collect the results of evaluating the expressions
	 *	into a datum array (argv) and pass this array to arrayFmgr()
	 * ----------------
	 */
	if (fcache->nargs != 0)
	{
		bool		argDone;

		if (fcache->nargs > MAXFMGRARGS)
			elog(ERROR, "ExecMakeFunctionResult: too many arguments");

		/*
		 * If the setArg in the fcache is set we have an argument
		 * returning a set of tuples (i.e. a nested dot expression).  We
		 * don't want to evaluate the arguments again until the function
		 * is done. hasSetArg will always be false until we eval the args
		 * for the first time. We should set this in the parser.
		 */
		if ((fcache->hasSetArg) && fcache->setArg != NULL)
		{
			argv[0] = (Datum) fcache->setArg;
			argDone = false;
		}
		else
			ExecEvalFuncArgs(fcache, econtext, arguments, argv, &argDone);

		if ((fcache->hasSetArg) && (argDone))
		{
			if (isDone)
				*isDone = true;
			return (Datum) NULL;
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
	 * right OID.  Also zero out the argv, since the real set doesn't take
	 * any arguments.
	 */
	if (((Func *) node)->funcid == SetEvalRegProcedure)
	{
		funcisset = true;
		if (fcache->setArg)
		{
			argv[0] = 0;

			((Func *) node)->funcid = (Oid) PointerGetDatum(fcache->setArg);

		}
		else
		{
			((Func *) node)->funcid = (Oid) argv[0];
			setFcache(node, argv[0], NIL, econtext);
			fcache = ((Func *) node)->func_fcache;
			fcache->setArg = (char *) argv[0];
			argv[0] = (Datum) 0;
		}
	}

	/* ----------------
	 *	 now return the value gotten by calling the function manager,
	 *	 passing the function the evaluated parameter values.
	 * ----------------
	 */
	if (fcache->language == SQLlanguageId)
	{
		Datum		result;

		Assert(funcNode);
		result = postquel_function(funcNode, (char **) argv, isNull, isDone);

		/*
		 * finagle the situation where we are iterating through all
		 * results in a nested dot function (whose argument function
		 * returns a set of tuples) and the current function finally
		 * finishes.  We need to get the next argument in the set and run
		 * the function all over again.  This is getting unclean.
		 */
		if ((*isDone) && (fcache->hasSetArg))
		{
			bool		argDone;

			ExecEvalFuncArgs(fcache, econtext, arguments, argv, &argDone);

			if (argDone)
			{
				fcache->setArg = (char *) NULL;
				*isDone = true;
				result = (Datum) NULL;
			}
			else
				result = postquel_function(funcNode,
										   (char **) argv,
										   isNull,
										   isDone);
		}
		if (funcisset)
		{

			/*
			 * reset the funcid so that next call to this routine will
			 * still recognize this func as a set. Note that for now we
			 * assume that the set function in pg_proc must be a Postquel
			 * function - the funcid is not reset below for C functions.
			 */
			((Func *) node)->funcid = SetEvalRegProcedure;

			/*
			 * If we're done with the results of this function, get rid of
			 * its func cache.
			 */
			if (*isDone)
			{
				((Func *) node)->func_fcache = NULL;
			}
		}
		return result;
	}
	else
	{
		int			i;

		if (isDone)
			*isDone = true;
		for (i = 0; i < fcache->nargs; i++)
			if (fcache->nullVect[i] == true)
				*isNull = true;

		return ((Datum) fmgr_c(&fcache->func, (FmgrValues *) argv, isNull));
	}
}


/* ----------------------------------------------------------------
 *		ExecEvalOper
 *		ExecEvalFunc
 *
 *		Evaluate the functional result of a list of arguments by calling the
 *		function manager.  Note that in the case of operator expressions, the
 *		optimizer had better have already replaced the operator OID with the
 *		appropriate function OID or we're hosed.
 *
 * old comments
 *		Presumably the function manager will not take null arguments, so we
 *		check for null arguments before sending the arguments to (fmgr).
 *
 *		Returns the value of the functional expression.
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

	/* ----------------
	 *	an opclause is a list (op args).  (I think)
	 *
	 *	we extract the oid of the function associated with
	 *	the op and then pass the work onto ExecMakeFunctionResult
	 *	which evaluates the arguments and returns the result of
	 *	calling the function on the evaluated arguments.
	 * ----------------
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

	/* -----------
	 *	call ExecMakeFunctionResult() with a dummy isDone that we ignore.
	 *	We don't have operator whose arguments are sets.
	 * -----------
	 */
	return
		ExecMakeFunctionResult((Node *) op, argList, econtext, isNull, &isDone);
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

	/* ----------------
	 *	an funcclause is a list (func args).  (I think)
	 *
	 *	we extract the oid of the function associated with
	 *	the func node and then pass the work onto ExecMakeFunctionResult
	 *	which evaluates the arguments and returns the result of
	 *	calling the function on the evaluated arguments.
	 *
	 *	this is nearly identical to the ExecEvalOper code.
	 * ----------------
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

	return
		ExecMakeFunctionResult((Node *) func, argList, econtext, isNull, isDone);
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
	Datum		expr_value;
	Node	   *clause;
	bool		isDone;

	clause = lfirst(notclause->args);

	/* ----------------
	 *	We don't iterate over sets in the quals, so pass in an isDone
	 *	flag, but ignore it.
	 * ----------------
	 */
	expr_value = ExecEvalExpr(clause, econtext, isNull, &isDone);

	/* ----------------
	 *	if the expression evaluates to null, then we just
	 *	cascade the null back to whoever called us.
	 * ----------------
	 */
	if (*isNull)
		return expr_value;

	/* ----------------
	 *	evaluation of 'not' is simple.. expr is false, then
	 *	return 'true' and vice versa.
	 * ----------------
	 */
	if (DatumGetInt32(expr_value) == 0)
		return (Datum) true;

	return (Datum) false;
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
	bool		IsNull;
	Datum		const_value = 0;

	IsNull = false;
	clauses = orExpr->args;

	/* ----------------
	 *	we use three valued logic functions here...
	 *	we evaluate each of the clauses in turn,
	 *	as soon as one is true we return that
	 *	value.	If none is true and  none of the
	 *	clauses evaluate to NULL we return
	 *	the value of the last clause evaluated (which
	 *	should be false) with *isNull set to false else
	 *	if none is true and at least one clause evaluated
	 *	to NULL we set *isNull flag to true -
	 * ----------------
	 */
	foreach(clause, clauses)
	{

		/* ----------------
		 *	We don't iterate over sets in the quals, so pass in an isDone
		 *	flag, but ignore it.
		 * ----------------
		 */
		const_value = ExecEvalExpr((Node *) lfirst(clause),
								   econtext,
								   isNull,
								   &isDone);

		/* ----------------
		 *	if the expression evaluates to null, then we
		 *	remember it in the local IsNull flag, if none of the
		 *	clauses are true then we need to set *isNull
		 *	to true again.
		 * ----------------
		 */
		if (*isNull)
		{
			IsNull = *isNull;

			/*
			 * Many functions don't (or can't!) check is an argument NULL
			 * or NOT_NULL and may return TRUE (1) with *isNull TRUE
			 * (an_int4_column <> 1: int4ne returns TRUE for NULLs). Not
			 * having time to fix function manager I want to fix OR: if we
			 * had 'x <> 1 OR x isnull' then TRUE, TRUE were returned by
			 * 'x <> 1' for NULL ... but ExecQualClause say that
			 * qualification *fails* if isnull is TRUE for all values
			 * returned by ExecEvalExpr. So, force this rule here: if
			 * isnull is TRUE then clause failed. Note: nullvalue() &
			 * nonnullvalue() always set isnull to FALSE for NULLs.    -
			 * vadim 09/22/97
			 */
			const_value = 0;
		}

		/* ----------------
		 *	 if we have a true result, then we return it.
		 * ----------------
		 */
		if (DatumGetInt32(const_value) != 0)
			return const_value;
	}

	/* IsNull is true if at least one clause evaluated to NULL */
	*isNull = IsNull;
	return const_value;
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
	Datum		const_value = 0;
	bool		isDone;
	bool		IsNull;

	IsNull = false;

	clauses = andExpr->args;

	/* ----------------
	 *	we evaluate each of the clauses in turn,
	 *	as soon as one is false we return that
	 *	value.	If none are false or NULL then we return
	 *	the value of the last clause evaluated, which
	 *	should be true.
	 * ----------------
	 */
	foreach(clause, clauses)
	{

		/* ----------------
		 *	We don't iterate over sets in the quals, so pass in an isDone
		 *	flag, but ignore it.
		 * ----------------
		 */
		const_value = ExecEvalExpr((Node *) lfirst(clause),
								   econtext,
								   isNull,
								   &isDone);

		/* ----------------
		 *	if the expression evaluates to null, then we
		 *	remember it in IsNull, if none of the clauses after
		 *	this evaluates to false we will have to set *isNull
		 *	to true again.
		 * ----------------
		 */
		if (*isNull)
			IsNull = *isNull;

		/* ----------------
		 *	 if we have a false result, then we return it, since the
		 *	 conjunction must be false.
		 * ----------------
		 */
		if (DatumGetInt32(const_value) == 0)
			return const_value;
	}

	*isNull = IsNull;
	return const_value;
}

/* ----------------------------------------------------------------
 *		ExecEvalExpr
 *
 *		Recursively evaluate a targetlist or qualification expression.
 *
 *		This routine is an inner loop routine and should be as fast
 *		as possible.
 *
 *		Node comparison functions were replaced by macros for speed and to plug
 *		memory leaks incurred by using the planner's Lispy stuff for
 *		comparisons.  Order of evaluation of node comparisons IS IMPORTANT;
 *		the macros do no checks.  Order of evaluation:
 *
 *		o an isnull check, largely to avoid coredumps since greg doubts this
 *		  routine is called with a null ptr anyway in proper operation, but is
 *		  not completely sure...
 *		o ExactNodeType checks.
 *		o clause checks or other checks where we look at the lfirst of something.
 * ----------------------------------------------------------------
 */
Datum
ExecEvalExpr(Node *expression,
			 ExprContext *econtext,
			 bool *isNull,
			 bool *isDone)
{
	Datum		retDatum = 0;

	*isNull = false;

	/*
	 * Some callers don't care about is done and only want 1 result.  They
	 * indicate this by passing NULL
	 */
	if (isDone)
		*isDone = true;

	/* ----------------
	 *	here we dispatch the work to the appropriate type
	 *	of function given the type of our expression.
	 * ----------------
	 */
	if (expression == NULL)
	{
		*isNull = true;
		return (Datum) true;
	}

	switch (nodeTag(expression))
	{
		case T_Var:
			retDatum = (Datum) ExecEvalVar((Var *) expression, econtext, isNull);
			break;
		case T_Const:
			{
				Const	   *con = (Const *) expression;

				if (con->constisnull)
					*isNull = true;
				retDatum = con->constvalue;
				break;
			}
		case T_Param:
			retDatum = (Datum) ExecEvalParam((Param *) expression, econtext, isNull);
			break;
		case T_Iter:
			retDatum = (Datum) ExecEvalIter((Iter *) expression,
											econtext,
											isNull,
											isDone);
			break;
		case T_Aggreg:
			retDatum = (Datum) ExecEvalAggreg((Aggreg *) expression,
											  econtext,
											  isNull);
			break;
		case T_ArrayRef:
			retDatum = (Datum) ExecEvalArrayRef((ArrayRef *) expression,
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
						retDatum = (Datum) ExecEvalOper(expr, econtext, isNull);
						break;
					case FUNC_EXPR:
						retDatum = (Datum) ExecEvalFunc(expr, econtext, isNull, isDone);
						break;
					case OR_EXPR:
						retDatum = (Datum) ExecEvalOr(expr, econtext, isNull);
						break;
					case AND_EXPR:
						retDatum = (Datum) ExecEvalAnd(expr, econtext, isNull);
						break;
					case NOT_EXPR:
						retDatum = (Datum) ExecEvalNot(expr, econtext, isNull);
						break;
					case SUBPLAN_EXPR:
						retDatum = (Datum) ExecSubPlan((SubPlan *) expr->oper, expr->args, econtext);
						break;
					default:
						elog(ERROR, "ExecEvalExpr: unknown expression type %d", expr->opType);
						break;
				}
				break;
			}
		default:
			elog(ERROR, "ExecEvalExpr: unknown expression type %d", nodeTag(expression));
			break;
	}

	return retDatum;
}

/* ----------------------------------------------------------------
 *					 ExecQual / ExecTargetList
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecQualClause
 *
 *		this is a workhorse for ExecQual.  ExecQual has to deal
 *		with a list of qualifications, so it passes each qualification
 *		in the list to this function one at a time.  ExecQualClause
 *		returns true when the qualification *fails* and false if
 *		the qualification succeeded (meaning we have to test the
 *		rest of the qualification)
 * ----------------------------------------------------------------
 */
static bool
ExecQualClause(Node *clause, ExprContext *econtext)
{
	Datum		expr_value;
	bool		isNull;
	bool		isDone;

	/* when there is a null clause, consider the qualification to be true */
	if (clause == NULL)
		return true;

	/*
	 * pass isDone, but ignore it.	We don't iterate over multiple returns
	 * in the qualifications.
	 */
	expr_value = (Datum)
		ExecEvalExpr(clause, econtext, &isNull, &isDone);

	/* ----------------
	 *	this is interesting behaviour here.  When a clause evaluates
	 *	to null, then we consider this as passing the qualification.
	 *	it seems kind of like, if the qual is NULL, then there's no
	 *	qual..
	 * ----------------
	 */
	if (isNull)
		return true;

	/* ----------------
	 *	remember, we return true when the qualification fails..
	 * ----------------
	 */
	if (DatumGetInt32(expr_value) == 0)
		return true;

	return false;
}

/* ----------------------------------------------------------------
 *		ExecQual
 *
 *		Evaluates a conjunctive boolean expression and returns t
 *		iff none of the subexpressions are false (or null).
 * ----------------------------------------------------------------
 */
bool
ExecQual(List *qual, ExprContext *econtext)
{
	List	   *clause;
	bool		result;

	/* ----------------
	 *	debugging stuff
	 * ----------------
	 */
	EV_printf("ExecQual: qual is ");
	EV_nodeDisplay(qual);
	EV_printf("\n");

	IncrProcessed();

	/* ----------------
	 *	return true immediately if no qual
	 * ----------------
	 */
	if (qual == NIL)
		return true;

	/* ----------------
	 *	a "qual" is a list of clauses.	To evaluate the
	 *	qual, we evaluate each of the clauses in the list.
	 *
	 *	ExecQualClause returns true when we know the qualification
	 *	*failed* so we just pass each clause in qual to it until
	 *	we know the qual failed or there are no more clauses.
	 * ----------------
	 */
	result = false;

	foreach(clause, qual)
	{
	 
	  
		result = ExecQualClause((Node *) lfirst(clause), econtext);
		if (result == true)
			break;
	}

	/* ----------------
	 *	if result is true, then it means a clause failed so we
	 *	return false.  if result is false then it means no clause
	 *	failed so we return true.
	 * ----------------
	 */
	if (result == true)
		return false;

	return true;
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
	char		nulls_array[64];
	bool		fjNullArray[64];
	bool	   *fjIsNull;
	char	   *null_head;
	List	   *tl;
	TargetEntry *tle;
	Node	   *expr;
	Resdom	   *resdom;
	AttrNumber	resind;
	Datum		constvalue;
	HeapTuple	newTuple;
	bool		isNull;

	/* ----------------
	 *	debugging stuff
	 * ----------------
	 */
	EV_printf("ExecTargetList: tl is ");
	EV_nodeDisplay(targetlist);
	EV_printf("\n");

	/* ----------------
	 * Return a dummy tuple if the targetlist is empty .
	 * the dummy tuple is necessary to differentiate
	 * between passing and failing the qualification.
	 * ----------------
	 */
	if (targetlist == NIL)
	{
		/* ----------------
		 *		I now think that the only time this makes
		 *		any sence is when we run a delete query.  Then
		 *		we need to return something other than nil
		 *		so we know to delete the tuple associated
		 *		with the saved tupleid.. see what ExecutePlan
		 *		does with the returned tuple.. -cim 9/21/89
		 *
		 *		It could also happen in queries like:
		 *			retrieve (foo.all) where bar.a = 3
		 *
		 *		is this a new phenomenon? it might cause bogus behavior
		 *		if we try to free this tuple later!! I put a hook in
		 *		ExecProject to watch out for this case -mer 24 Aug 1992
		 * ----------------
		 */
		CXT1_printf("ExecTargetList: context is %d\n", CurrentMemoryContext);
		*isDone = true;
		return (HeapTuple) true;
	}

	/* ----------------
	 *	allocate an array of char's to hold the "null" information
	 *	only if we have a really large targetlist.	otherwise we use
	 *	the stack.
	 * ----------------
	 */
	if (nodomains > 64)
	{
		null_head = (char *) palloc(nodomains + 1);
		fjIsNull = (bool *) palloc(nodomains + 1);
	}
	else
	{
		null_head = &nulls_array[0];
		fjIsNull = &fjNullArray[0];
	}

	/* ----------------
	 *	evaluate all the expressions in the target list
	 * ----------------
	 */
	EV_printf("ExecTargetList: setting target list values\n");

	*isDone = true;
	foreach(tl, targetlist)
	{
		/* ----------------
		 *	  remember, a target list is a list of lists:
		 *
		 *		((<resdom | fjoin> expr) (<resdom | fjoin> expr) ...)
		 *
		 *	  tl is a pointer to successive cdr's of the targetlist
		 *	  tle is a pointer to the target list entry in tl
		 * ----------------
		 */
		tle = lfirst(tl);

		if (tle->resdom != NULL)
		{
			expr = tle->expr;
			resdom = tle->resdom;
			resind = resdom->resno - 1;
			constvalue = (Datum) ExecEvalExpr(expr,
											  econtext,
											  &isNull,
											  isDone);

			if ((IsA(expr, Iter)) && (*isDone))
				return (HeapTuple) NULL;

			values[resind] = constvalue;

			if (!isNull)
				null_head[resind] = ' ';
			else
				null_head[resind] = 'n';
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
			if (*isDone)
				return (HeapTuple) NULL;

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
#if 0							/* what is this?? */
				Node	   *outernode = lfirst(fjTlist);

				fjRes = (Resdom *) outernode->iterexpr;
#endif
				resind = fjRes->resno - 1;
				if (fjIsNull[curNode])
				{
					null_head[resind] = 'n';
				}
				else
				{
					null_head[resind] = ' ';
					values[resind] = results[curNode];
				}
			}
		}
	}

	/* ----------------
	 *	form the new result tuple (in the "normal" context)
	 * ----------------
	 */
	newTuple = (HeapTuple)
		heap_formtuple(targettype, values, null_head);

	/* ----------------
	 *	free the nulls array if we allocated one..
	 * ----------------
	 */
	if (nodomains > 64)
		pfree(null_head);

	return
		newTuple;
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

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	if (projInfo == NULL)
		return (TupleTableSlot *) NULL;

	/* ----------------
	 *	get the projection info we want
	 * ----------------
	 */
	slot = projInfo->pi_slot;
	targetlist = projInfo->pi_targetlist;
	len = projInfo->pi_len;
	tupType = slot->ttc_tupleDescriptor;

	tupValue = projInfo->pi_tupValue;
	econtext = projInfo->pi_exprContext;

	if (targetlist == NIL)
	{
		*isDone = true;
		return (TupleTableSlot *) NULL;
	}

	/* ----------------
	 *	form a new (result) tuple
	 * ----------------
	 */
	newTuple = ExecTargetList(targetlist,
							  len,
							  tupType,
							  tupValue,
							  econtext,
							  isDone);

	/* ----------------
	 *	store the tuple in the projection slot and return the slot.
	 *
	 *	If there's no projection target list we don't want to pfree
	 *	the bogus tuple that ExecTargetList passes back to us.
	 *		 -mer 24 Aug 1992
	 * ----------------
	 */
	return (TupleTableSlot *)
		ExecStoreTuple(newTuple,/* tuple to store */
					   slot,	/* slot to store in */
					   InvalidBuffer,	/* tuple has no buffer */
					   true);
}
