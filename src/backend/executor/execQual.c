/*-------------------------------------------------------------------------
 *
 * execQual.c
 *	  Routines to evaluate qualification and targetlist expressions
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execQual.c
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		ExecEvalExpr	- (now a macro) evaluate an expression, return a datum
 *		ExecEvalExprSwitchContext - same, but switch into eval memory context
 *		ExecQual		- return true/false if qualification is satisfied
 *		ExecProject		- form a new tuple by projecting the given tuple
 *
 *	 NOTES
 *		The more heavily used ExecEvalExpr routines, such as ExecEvalScalarVar,
 *		are hotspots. Making these faster will speed up the entire system.
 *
 *		ExecProject() is used to make tuple projections.  Rather then
 *		trying to speed it up, the execution plan should be pre-processed
 *		to facilitate attribute sharing between nodes wherever possible,
 *		instead of doing needless copying.	-cim 5/31/91
 *
 *		During expression evaluation, we check_stack_depth only in
 *		ExecMakeFunctionResult (and substitute routines) rather than at every
 *		single node.  This is a compromise that trades off precision of the
 *		stack limit setting to gain speed.
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/tupconvert.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_type.h"
#include "commands/typecmds.h"
#include "executor/execdebug.h"
#include "executor/nodeSubplan.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/planner.h"
#include "parser/parse_coerce.h"
#include "pgstat.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/typcache.h"
#include "utils/xml.h"


/* static function decls */
static Datum ExecEvalArrayRef(ArrayRefExprState *astate,
				 ExprContext *econtext,
				 bool *isNull, ExprDoneCond *isDone);
static bool isAssignmentIndirectionExpr(ExprState *exprstate);
static Datum ExecEvalAggref(AggrefExprState *aggref,
			   ExprContext *econtext,
			   bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalWindowFunc(WindowFuncExprState *wfunc,
				   ExprContext *econtext,
				   bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalScalarVar(ExprState *exprstate, ExprContext *econtext,
				  bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalScalarVarFast(ExprState *exprstate, ExprContext *econtext,
					  bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalWholeRowVar(WholeRowVarExprState *wrvstate,
					ExprContext *econtext,
					bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalWholeRowFast(WholeRowVarExprState *wrvstate,
					 ExprContext *econtext,
					 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalWholeRowSlow(WholeRowVarExprState *wrvstate,
					 ExprContext *econtext,
					 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalConst(ExprState *exprstate, ExprContext *econtext,
			  bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalParamExec(ExprState *exprstate, ExprContext *econtext,
				  bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalParamExtern(ExprState *exprstate, ExprContext *econtext,
					bool *isNull, ExprDoneCond *isDone);
static void init_fcache(Oid foid, Oid input_collation, FuncExprState *fcache,
			MemoryContext fcacheCxt, bool needDescForSets);
static void ShutdownFuncExpr(Datum arg);
static TupleDesc get_cached_rowtype(Oid type_id, int32 typmod,
				   TupleDesc *cache_field, ExprContext *econtext);
static void ShutdownTupleDescRef(Datum arg);
static ExprDoneCond ExecEvalFuncArgs(FunctionCallInfo fcinfo,
				 List *argList, ExprContext *econtext);
static void ExecPrepareTuplestoreResult(FuncExprState *fcache,
							ExprContext *econtext,
							Tuplestorestate *resultStore,
							TupleDesc resultDesc);
static void tupledesc_match(TupleDesc dst_tupdesc, TupleDesc src_tupdesc);
static Datum ExecMakeFunctionResult(FuncExprState *fcache,
					   ExprContext *econtext,
					   bool *isNull,
					   ExprDoneCond *isDone);
static Datum ExecMakeFunctionResultNoSets(FuncExprState *fcache,
							 ExprContext *econtext,
							 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalFunc(FuncExprState *fcache, ExprContext *econtext,
			 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalOper(FuncExprState *fcache, ExprContext *econtext,
			 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalDistinct(FuncExprState *fcache, ExprContext *econtext,
				 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalScalarArrayOp(ScalarArrayOpExprState *sstate,
					  ExprContext *econtext,
					  bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalNot(BoolExprState *notclause, ExprContext *econtext,
			bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalOr(BoolExprState *orExpr, ExprContext *econtext,
		   bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalAnd(BoolExprState *andExpr, ExprContext *econtext,
			bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalConvertRowtype(ConvertRowtypeExprState *cstate,
					   ExprContext *econtext,
					   bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalCase(CaseExprState *caseExpr, ExprContext *econtext,
			 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalCaseTestExpr(ExprState *exprstate,
					 ExprContext *econtext,
					 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalArray(ArrayExprState *astate,
			  ExprContext *econtext,
			  bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalRow(RowExprState *rstate,
			ExprContext *econtext,
			bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalRowCompare(RowCompareExprState *rstate,
				   ExprContext *econtext,
				   bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalCoalesce(CoalesceExprState *coalesceExpr,
				 ExprContext *econtext,
				 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalMinMax(MinMaxExprState *minmaxExpr,
			   ExprContext *econtext,
			   bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalXml(XmlExprState *xmlExpr, ExprContext *econtext,
			bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalNullIf(FuncExprState *nullIfExpr,
			   ExprContext *econtext,
			   bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalNullTest(NullTestState *nstate,
				 ExprContext *econtext,
				 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalBooleanTest(GenericExprState *bstate,
					ExprContext *econtext,
					bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalCoerceToDomain(CoerceToDomainState *cstate,
					   ExprContext *econtext,
					   bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalCoerceToDomainValue(ExprState *exprstate,
							ExprContext *econtext,
							bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalFieldSelect(FieldSelectState *fstate,
					ExprContext *econtext,
					bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalFieldStore(FieldStoreState *fstate,
				   ExprContext *econtext,
				   bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalRelabelType(GenericExprState *exprstate,
					ExprContext *econtext,
					bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalCoerceViaIO(CoerceViaIOState *iostate,
					ExprContext *econtext,
					bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalArrayCoerceExpr(ArrayCoerceExprState *astate,
						ExprContext *econtext,
						bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalCurrentOfExpr(ExprState *exprstate, ExprContext *econtext,
					  bool *isNull, ExprDoneCond *isDone);


/* ----------------------------------------------------------------
 *		ExecEvalExpr routines
 *
 *		Recursively evaluate a targetlist or qualification expression.
 *
 * Each of the following routines having the signature
 *		Datum ExecEvalFoo(ExprState *expression,
 *						  ExprContext *econtext,
 *						  bool *isNull,
 *						  ExprDoneCond *isDone);
 * is responsible for evaluating one type or subtype of ExprState node.
 * They are normally called via the ExecEvalExpr macro, which makes use of
 * the function pointer set up when the ExprState node was built by
 * ExecInitExpr.  (In some cases, we change this pointer later to avoid
 * re-executing one-time overhead.)
 *
 * Note: for notational simplicity we declare these functions as taking the
 * specific type of ExprState that they work on.  This requires casting when
 * assigning the function pointer in ExecInitExpr.	Be careful that the
 * function signature is declared correctly, because the cast suppresses
 * automatic checking!
 *
 *
 * All these functions share this calling convention:
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
 * do the switch in an outer loop.	We do not do the switch in these routines
 * because it'd be a waste of cycles during nested expression evaluation.
 * ----------------------------------------------------------------
 */


/*----------
 *	  ExecEvalArrayRef
 *
 *	   This function takes an ArrayRef and returns the extracted Datum
 *	   if it's a simple reference, or the modified array value if it's
 *	   an array assignment (i.e., array element or slice insertion).
 *
 * NOTE: if we get a NULL result from a subscript expression, we return NULL
 * when it's an array reference, or raise an error when it's an assignment.
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
	bool		eisnull;
	ListCell   *l;
	int			i = 0,
				j = 0;
	IntArray	upper,
				lower;
	int		   *lIndex;

	array_source = (ArrayType *)
		DatumGetPointer(ExecEvalExpr(astate->refexpr,
									 econtext,
									 isNull,
									 isDone));

	/*
	 * If refexpr yields NULL, and it's a fetch, then result is NULL. In the
	 * assignment case, we'll cons up something below.
	 */
	if (*isNull)
	{
		if (isDone && *isDone == ExprEndResult)
			return (Datum) NULL;	/* end of set result */
		if (!isAssignment)
			return (Datum) NULL;
	}

	foreach(l, astate->refupperindexpr)
	{
		ExprState  *eltstate = (ExprState *) lfirst(l);

		if (i >= MAXDIM)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
							i + 1, MAXDIM)));

		upper.indx[i++] = DatumGetInt32(ExecEvalExpr(eltstate,
													 econtext,
													 &eisnull,
													 NULL));
		/* If any index expr yields NULL, result is NULL or error */
		if (eisnull)
		{
			if (isAssignment)
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				  errmsg("array subscript in assignment must not be null")));
			*isNull = true;
			return (Datum) NULL;
		}
	}

	if (astate->reflowerindexpr != NIL)
	{
		foreach(l, astate->reflowerindexpr)
		{
			ExprState  *eltstate = (ExprState *) lfirst(l);

			if (j >= MAXDIM)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
								j + 1, MAXDIM)));

			lower.indx[j++] = DatumGetInt32(ExecEvalExpr(eltstate,
														 econtext,
														 &eisnull,
														 NULL));
			/* If any index expr yields NULL, result is NULL or error */
			if (eisnull)
			{
				if (isAssignment)
					ereport(ERROR,
							(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							 errmsg("array subscript in assignment must not be null")));
				*isNull = true;
				return (Datum) NULL;
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
		Datum		sourceData;
		Datum		save_datum;
		bool		save_isNull;

		/*
		 * We might have a nested-assignment situation, in which the
		 * refassgnexpr is itself a FieldStore or ArrayRef that needs to
		 * obtain and modify the previous value of the array element or slice
		 * being replaced.	If so, we have to extract that value from the
		 * array and pass it down via the econtext's caseValue.  It's safe to
		 * reuse the CASE mechanism because there cannot be a CASE between
		 * here and where the value would be needed, and an array assignment
		 * can't be within a CASE either.  (So saving and restoring the
		 * caseValue is just paranoia, but let's do it anyway.)
		 *
		 * Since fetching the old element might be a nontrivial expense, do it
		 * only if the argument appears to actually need it.
		 */
		save_datum = econtext->caseValue_datum;
		save_isNull = econtext->caseValue_isNull;

		if (isAssignmentIndirectionExpr(astate->refassgnexpr))
		{
			if (*isNull)
			{
				/* whole array is null, so any element or slice is too */
				econtext->caseValue_datum = (Datum) 0;
				econtext->caseValue_isNull = true;
			}
			else if (lIndex == NULL)
			{
				econtext->caseValue_datum = array_ref(array_source, i,
													  upper.indx,
													  astate->refattrlength,
													  astate->refelemlength,
													  astate->refelembyval,
													  astate->refelemalign,
												&econtext->caseValue_isNull);
			}
			else
			{
				resultArray = array_get_slice(array_source, i,
											  upper.indx, lower.indx,
											  astate->refattrlength,
											  astate->refelemlength,
											  astate->refelembyval,
											  astate->refelemalign);
				econtext->caseValue_datum = PointerGetDatum(resultArray);
				econtext->caseValue_isNull = false;
			}
		}
		else
		{
			/* argument shouldn't need caseValue, but for safety set it null */
			econtext->caseValue_datum = (Datum) 0;
			econtext->caseValue_isNull = true;
		}

		/*
		 * Evaluate the value to be assigned into the array.
		 */
		sourceData = ExecEvalExpr(astate->refassgnexpr,
								  econtext,
								  &eisnull,
								  NULL);

		econtext->caseValue_datum = save_datum;
		econtext->caseValue_isNull = save_isNull;

		/*
		 * For an assignment to a fixed-length array type, both the original
		 * array and the value to be assigned into it must be non-NULL, else
		 * we punt and return the original array.
		 */
		if (astate->refattrlength > 0)	/* fixed-length array? */
			if (eisnull || *isNull)
				return PointerGetDatum(array_source);

		/*
		 * For assignment to varlena arrays, we handle a NULL original array
		 * by substituting an empty (zero-dimensional) array; insertion of the
		 * new element will result in a singleton array value.	It does not
		 * matter whether the new element is NULL.
		 */
		if (*isNull)
		{
			array_source = construct_empty_array(arrayRef->refelemtype);
			*isNull = false;
		}

		if (lIndex == NULL)
			resultArray = array_set(array_source, i,
									upper.indx,
									sourceData,
									eisnull,
									astate->refattrlength,
									astate->refelemlength,
									astate->refelembyval,
									astate->refelemalign);
		else
			resultArray = array_set_slice(array_source, i,
										  upper.indx, lower.indx,
								   (ArrayType *) DatumGetPointer(sourceData),
										  eisnull,
										  astate->refattrlength,
										  astate->refelemlength,
										  astate->refelembyval,
										  astate->refelemalign);
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
									  astate->refelemalign);
		return PointerGetDatum(resultArray);
	}
}

/*
 * Helper for ExecEvalArrayRef: is expr a nested FieldStore or ArrayRef
 * that might need the old element value passed down?
 *
 * (We could use this in ExecEvalFieldStore too, but in that case passing
 * the old value is so cheap there's no need.)
 */
static bool
isAssignmentIndirectionExpr(ExprState *exprstate)
{
	if (exprstate == NULL)
		return false;			/* just paranoia */
	if (IsA(exprstate, FieldStoreState))
	{
		FieldStore *fstore = (FieldStore *) exprstate->expr;

		if (fstore->arg && IsA(fstore->arg, CaseTestExpr))
			return true;
	}
	else if (IsA(exprstate, ArrayRefExprState))
	{
		ArrayRef   *arrayRef = (ArrayRef *) exprstate->expr;

		if (arrayRef->refexpr && IsA(arrayRef->refexpr, CaseTestExpr))
			return true;
	}
	return false;
}

/* ----------------------------------------------------------------
 *		ExecEvalAggref
 *
 *		Returns a Datum whose value is the value of the precomputed
 *		aggregate found in the given expression context.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalAggref(AggrefExprState *aggref, ExprContext *econtext,
			   bool *isNull, ExprDoneCond *isDone)
{
	if (isDone)
		*isDone = ExprSingleResult;

	if (econtext->ecxt_aggvalues == NULL)		/* safety check */
		elog(ERROR, "no aggregates in this expression context");

	*isNull = econtext->ecxt_aggnulls[aggref->aggno];
	return econtext->ecxt_aggvalues[aggref->aggno];
}

/* ----------------------------------------------------------------
 *		ExecEvalWindowFunc
 *
 *		Returns a Datum whose value is the value of the precomputed
 *		window function found in the given expression context.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalWindowFunc(WindowFuncExprState *wfunc, ExprContext *econtext,
				   bool *isNull, ExprDoneCond *isDone)
{
	if (isDone)
		*isDone = ExprSingleResult;

	if (econtext->ecxt_aggvalues == NULL)		/* safety check */
		elog(ERROR, "no window functions in this expression context");

	*isNull = econtext->ecxt_aggnulls[wfunc->wfuncno];
	return econtext->ecxt_aggvalues[wfunc->wfuncno];
}

/* ----------------------------------------------------------------
 *		ExecEvalScalarVar
 *
 *		Returns a Datum whose value is the value of a scalar (not whole-row)
 *		range variable with respect to given expression context.
 *
 * Note: ExecEvalScalarVar is executed only the first time through in a given
 * plan; it changes the ExprState's function pointer to pass control directly
 * to ExecEvalScalarVarFast after making one-time checks.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalScalarVar(ExprState *exprstate, ExprContext *econtext,
				  bool *isNull, ExprDoneCond *isDone)
{
	Var		   *variable = (Var *) exprstate->expr;
	TupleTableSlot *slot;
	AttrNumber	attnum;

	if (isDone)
		*isDone = ExprSingleResult;

	/* Get the input slot and attribute number we want */
	switch (variable->varno)
	{
		case INNER_VAR: /* get the tuple from the inner node */
			slot = econtext->ecxt_innertuple;
			break;

		case OUTER_VAR: /* get the tuple from the outer node */
			slot = econtext->ecxt_outertuple;
			break;

			/* INDEX_VAR is handled by default case */

		default:				/* get the tuple from the relation being
								 * scanned */
			slot = econtext->ecxt_scantuple;
			break;
	}

	attnum = variable->varattno;

	/* This was checked by ExecInitExpr */
	Assert(attnum != InvalidAttrNumber);

	/*
	 * If it's a user attribute, check validity (bogus system attnums will be
	 * caught inside slot_getattr).  What we have to check for here is the
	 * possibility of an attribute having been changed in type since the plan
	 * tree was created.  Ideally the plan will get invalidated and not
	 * re-used, but just in case, we keep these defenses.  Fortunately it's
	 * sufficient to check once on the first time through.
	 *
	 * Note: we allow a reference to a dropped attribute.  slot_getattr will
	 * force a NULL result in such cases.
	 *
	 * Note: ideally we'd check typmod as well as typid, but that seems
	 * impractical at the moment: in many cases the tupdesc will have been
	 * generated by ExecTypeFromTL(), and that can't guarantee to generate an
	 * accurate typmod in all cases, because some expression node types don't
	 * carry typmod.
	 */
	if (attnum > 0)
	{
		TupleDesc	slot_tupdesc = slot->tts_tupleDescriptor;
		Form_pg_attribute attr;

		if (attnum > slot_tupdesc->natts)		/* should never happen */
			elog(ERROR, "attribute number %d exceeds number of columns %d",
				 attnum, slot_tupdesc->natts);

		attr = slot_tupdesc->attrs[attnum - 1];

		/* can't check type if dropped, since atttypid is probably 0 */
		if (!attr->attisdropped)
		{
			if (variable->vartype != attr->atttypid)
				ereport(ERROR,
						(errmsg("attribute %d has wrong type", attnum),
						 errdetail("Table has type %s, but query expects %s.",
								   format_type_be(attr->atttypid),
								   format_type_be(variable->vartype))));
		}
	}

	/* Skip the checking on future executions of node */
	exprstate->evalfunc = ExecEvalScalarVarFast;

	/* Fetch the value from the slot */
	return slot_getattr(slot, attnum, isNull);
}

/* ----------------------------------------------------------------
 *		ExecEvalScalarVarFast
 *
 *		Returns a Datum for a scalar variable.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalScalarVarFast(ExprState *exprstate, ExprContext *econtext,
					  bool *isNull, ExprDoneCond *isDone)
{
	Var		   *variable = (Var *) exprstate->expr;
	TupleTableSlot *slot;
	AttrNumber	attnum;

	if (isDone)
		*isDone = ExprSingleResult;

	/* Get the input slot and attribute number we want */
	switch (variable->varno)
	{
		case INNER_VAR: /* get the tuple from the inner node */
			slot = econtext->ecxt_innertuple;
			break;

		case OUTER_VAR: /* get the tuple from the outer node */
			slot = econtext->ecxt_outertuple;
			break;

			/* INDEX_VAR is handled by default case */

		default:				/* get the tuple from the relation being
								 * scanned */
			slot = econtext->ecxt_scantuple;
			break;
	}

	attnum = variable->varattno;

	/* Fetch the value from the slot */
	return slot_getattr(slot, attnum, isNull);
}

/* ----------------------------------------------------------------
 *		ExecEvalWholeRowVar
 *
 *		Returns a Datum whose value is the value of a whole-row range
 *		variable with respect to given expression context.
 *
 * Note: ExecEvalWholeRowVar is executed only the first time through in a
 * given plan; it changes the ExprState's function pointer to pass control
 * directly to ExecEvalWholeRowFast or ExecEvalWholeRowSlow after making
 * one-time checks.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalWholeRowVar(WholeRowVarExprState *wrvstate, ExprContext *econtext,
					bool *isNull, ExprDoneCond *isDone)
{
	Var		   *variable = (Var *) wrvstate->xprstate.expr;
	TupleTableSlot *slot;
	TupleDesc	slot_tupdesc;
	bool		needslow = false;

	if (isDone)
		*isDone = ExprSingleResult;

	/* This was checked by ExecInitExpr */
	Assert(variable->varattno == InvalidAttrNumber);

	/* Get the input slot we want */
	switch (variable->varno)
	{
		case INNER_VAR: /* get the tuple from the inner node */
			slot = econtext->ecxt_innertuple;
			break;

		case OUTER_VAR: /* get the tuple from the outer node */
			slot = econtext->ecxt_outertuple;
			break;

			/* INDEX_VAR is handled by default case */

		default:				/* get the tuple from the relation being
								 * scanned */
			slot = econtext->ecxt_scantuple;
			break;
	}

	/*
	 * If the input tuple came from a subquery, it might contain "resjunk"
	 * columns (such as GROUP BY or ORDER BY columns), which we don't want to
	 * keep in the whole-row result.  We can get rid of such columns by
	 * passing the tuple through a JunkFilter --- but to make one, we have to
	 * lay our hands on the subquery's targetlist.  Fortunately, there are not
	 * very many cases where this can happen, and we can identify all of them
	 * by examining our parent PlanState.  We assume this is not an issue in
	 * standalone expressions that don't have parent plans.  (Whole-row Vars
	 * can occur in such expressions, but they will always be referencing
	 * table rows.)
	 */
	if (wrvstate->parent)
	{
		PlanState  *subplan = NULL;

		switch (nodeTag(wrvstate->parent))
		{
			case T_SubqueryScanState:
				subplan = ((SubqueryScanState *) wrvstate->parent)->subplan;
				break;
			case T_CteScanState:
				subplan = ((CteScanState *) wrvstate->parent)->cteplanstate;
				break;
			default:
				break;
		}

		if (subplan)
		{
			bool		junk_filter_needed = false;
			ListCell   *tlist;

			/* Detect whether subplan tlist actually has any junk columns */
			foreach(tlist, subplan->plan->targetlist)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(tlist);

				if (tle->resjunk)
				{
					junk_filter_needed = true;
					break;
				}
			}

			/* If so, build the junkfilter in the query memory context */
			if (junk_filter_needed)
			{
				MemoryContext oldcontext;

				oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
				wrvstate->wrv_junkFilter =
					ExecInitJunkFilter(subplan->plan->targetlist,
									   ExecGetResultType(subplan)->tdhasoid,
							ExecInitExtraTupleSlot(wrvstate->parent->state));
				MemoryContextSwitchTo(oldcontext);
			}
		}
	}

	/* Apply the junkfilter if any */
	if (wrvstate->wrv_junkFilter != NULL)
		slot = ExecFilterJunk(wrvstate->wrv_junkFilter, slot);

	slot_tupdesc = slot->tts_tupleDescriptor;

	/*
	 * If it's a RECORD Var, we'll use the slot's type ID info.  It's likely
	 * that the slot's type is also RECORD; if so, make sure it's been
	 * "blessed", so that the Datum can be interpreted later.
	 *
	 * If the Var identifies a named composite type, we must check that the
	 * actual tuple type is compatible with it.
	 */
	if (variable->vartype == RECORDOID)
	{
		if (slot_tupdesc->tdtypeid == RECORDOID &&
			slot_tupdesc->tdtypmod < 0)
			assign_record_type_typmod(slot_tupdesc);
	}
	else
	{
		TupleDesc	var_tupdesc;
		int			i;

		/*
		 * We really only care about numbers of attributes and data types.
		 * Also, we can ignore type mismatch on columns that are dropped in
		 * the destination type, so long as (1) the physical storage matches
		 * or (2) the actual column value is NULL.	Case (1) is helpful in
		 * some cases involving out-of-date cached plans, while case (2) is
		 * expected behavior in situations such as an INSERT into a table with
		 * dropped columns (the planner typically generates an INT4 NULL
		 * regardless of the dropped column type).	If we find a dropped
		 * column and cannot verify that case (1) holds, we have to use
		 * ExecEvalWholeRowSlow to check (2) for each row.
		 */
		var_tupdesc = lookup_rowtype_tupdesc(variable->vartype, -1);

		if (var_tupdesc->natts != slot_tupdesc->natts)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table row type and query-specified row type do not match"),
					 errdetail_plural("Table row contains %d attribute, but query expects %d.",
				   "Table row contains %d attributes, but query expects %d.",
									  slot_tupdesc->natts,
									  slot_tupdesc->natts,
									  var_tupdesc->natts)));

		for (i = 0; i < var_tupdesc->natts; i++)
		{
			Form_pg_attribute vattr = var_tupdesc->attrs[i];
			Form_pg_attribute sattr = slot_tupdesc->attrs[i];

			if (vattr->atttypid == sattr->atttypid)
				continue;		/* no worries */
			if (!vattr->attisdropped)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table row type and query-specified row type do not match"),
						 errdetail("Table has type %s at ordinal position %d, but query expects %s.",
								   format_type_be(sattr->atttypid),
								   i + 1,
								   format_type_be(vattr->atttypid))));

			if (vattr->attlen != sattr->attlen ||
				vattr->attalign != sattr->attalign)
				needslow = true;	/* need runtime check for null */
		}

		ReleaseTupleDesc(var_tupdesc);
	}

	/* Skip the checking on future executions of node */
	if (needslow)
		wrvstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalWholeRowSlow;
	else
		wrvstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalWholeRowFast;

	/* Fetch the value */
	return (*wrvstate->xprstate.evalfunc) ((ExprState *) wrvstate, econtext,
										   isNull, isDone);
}

/* ----------------------------------------------------------------
 *		ExecEvalWholeRowFast
 *
 *		Returns a Datum for a whole-row variable.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalWholeRowFast(WholeRowVarExprState *wrvstate, ExprContext *econtext,
					 bool *isNull, ExprDoneCond *isDone)
{
	Var		   *variable = (Var *) wrvstate->xprstate.expr;
	TupleTableSlot *slot;
	HeapTuple	tuple;
	TupleDesc	tupleDesc;
	HeapTupleHeader dtuple;

	if (isDone)
		*isDone = ExprSingleResult;
	*isNull = false;

	/* Get the input slot we want */
	switch (variable->varno)
	{
		case INNER_VAR: /* get the tuple from the inner node */
			slot = econtext->ecxt_innertuple;
			break;

		case OUTER_VAR: /* get the tuple from the outer node */
			slot = econtext->ecxt_outertuple;
			break;

			/* INDEX_VAR is handled by default case */

		default:				/* get the tuple from the relation being
								 * scanned */
			slot = econtext->ecxt_scantuple;
			break;
	}

	/* Apply the junkfilter if any */
	if (wrvstate->wrv_junkFilter != NULL)
		slot = ExecFilterJunk(wrvstate->wrv_junkFilter, slot);

	tuple = ExecFetchSlotTuple(slot);
	tupleDesc = slot->tts_tupleDescriptor;

	/*
	 * We have to make a copy of the tuple so we can safely insert the Datum
	 * overhead fields, which are not set in on-disk tuples.
	 */
	dtuple = (HeapTupleHeader) palloc(tuple->t_len);
	memcpy((char *) dtuple, (char *) tuple->t_data, tuple->t_len);

	HeapTupleHeaderSetDatumLength(dtuple, tuple->t_len);

	/*
	 * If the Var identifies a named composite type, label the tuple with that
	 * type; otherwise use what is in the tupleDesc.
	 */
	if (variable->vartype != RECORDOID)
	{
		HeapTupleHeaderSetTypeId(dtuple, variable->vartype);
		HeapTupleHeaderSetTypMod(dtuple, variable->vartypmod);
	}
	else
	{
		HeapTupleHeaderSetTypeId(dtuple, tupleDesc->tdtypeid);
		HeapTupleHeaderSetTypMod(dtuple, tupleDesc->tdtypmod);
	}

	return PointerGetDatum(dtuple);
}

/* ----------------------------------------------------------------
 *		ExecEvalWholeRowSlow
 *
 *		Returns a Datum for a whole-row variable, in the "slow" case where
 *		we can't just copy the subplan's output.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalWholeRowSlow(WholeRowVarExprState *wrvstate, ExprContext *econtext,
					 bool *isNull, ExprDoneCond *isDone)
{
	Var		   *variable = (Var *) wrvstate->xprstate.expr;
	TupleTableSlot *slot;
	HeapTuple	tuple;
	TupleDesc	tupleDesc;
	TupleDesc	var_tupdesc;
	HeapTupleHeader dtuple;
	int			i;

	if (isDone)
		*isDone = ExprSingleResult;
	*isNull = false;

	/* Get the input slot we want */
	switch (variable->varno)
	{
		case INNER_VAR: /* get the tuple from the inner node */
			slot = econtext->ecxt_innertuple;
			break;

		case OUTER_VAR: /* get the tuple from the outer node */
			slot = econtext->ecxt_outertuple;
			break;

			/* INDEX_VAR is handled by default case */

		default:				/* get the tuple from the relation being
								 * scanned */
			slot = econtext->ecxt_scantuple;
			break;
	}

	/* Apply the junkfilter if any */
	if (wrvstate->wrv_junkFilter != NULL)
		slot = ExecFilterJunk(wrvstate->wrv_junkFilter, slot);

	tuple = ExecFetchSlotTuple(slot);
	tupleDesc = slot->tts_tupleDescriptor;

	Assert(variable->vartype != RECORDOID);
	var_tupdesc = lookup_rowtype_tupdesc(variable->vartype, -1);

	/* Check to see if any dropped attributes are non-null */
	for (i = 0; i < var_tupdesc->natts; i++)
	{
		Form_pg_attribute vattr = var_tupdesc->attrs[i];
		Form_pg_attribute sattr = tupleDesc->attrs[i];

		if (!vattr->attisdropped)
			continue;			/* already checked non-dropped cols */
		if (heap_attisnull(tuple, i + 1))
			continue;			/* null is always okay */
		if (vattr->attlen != sattr->attlen ||
			vattr->attalign != sattr->attalign)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table row type and query-specified row type do not match"),
					 errdetail("Physical storage mismatch on dropped attribute at ordinal position %d.",
							   i + 1)));
	}

	/*
	 * We have to make a copy of the tuple so we can safely insert the Datum
	 * overhead fields, which are not set in on-disk tuples.
	 */
	dtuple = (HeapTupleHeader) palloc(tuple->t_len);
	memcpy((char *) dtuple, (char *) tuple->t_data, tuple->t_len);

	HeapTupleHeaderSetDatumLength(dtuple, tuple->t_len);
	HeapTupleHeaderSetTypeId(dtuple, variable->vartype);
	HeapTupleHeaderSetTypMod(dtuple, variable->vartypmod);

	ReleaseTupleDesc(var_tupdesc);

	return PointerGetDatum(dtuple);
}

/* ----------------------------------------------------------------
 *		ExecEvalConst
 *
 *		Returns the value of a constant.
 *
 *		Note that for pass-by-ref datatypes, we return a pointer to the
 *		actual constant node.  This is one of the reasons why functions
 *		must treat their input arguments as read-only.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalConst(ExprState *exprstate, ExprContext *econtext,
			  bool *isNull, ExprDoneCond *isDone)
{
	Const	   *con = (Const *) exprstate->expr;

	if (isDone)
		*isDone = ExprSingleResult;

	*isNull = con->constisnull;
	return con->constvalue;
}

/* ----------------------------------------------------------------
 *		ExecEvalParamExec
 *
 *		Returns the value of a PARAM_EXEC parameter.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalParamExec(ExprState *exprstate, ExprContext *econtext,
				  bool *isNull, ExprDoneCond *isDone)
{
	Param	   *expression = (Param *) exprstate->expr;
	int			thisParamId = expression->paramid;
	ParamExecData *prm;

	if (isDone)
		*isDone = ExprSingleResult;

	/*
	 * PARAM_EXEC params (internal executor parameters) are stored in the
	 * ecxt_param_exec_vals array, and can be accessed by array index.
	 */
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

/* ----------------------------------------------------------------
 *		ExecEvalParamExtern
 *
 *		Returns the value of a PARAM_EXTERN parameter.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalParamExtern(ExprState *exprstate, ExprContext *econtext,
					bool *isNull, ExprDoneCond *isDone)
{
	Param	   *expression = (Param *) exprstate->expr;
	int			thisParamId = expression->paramid;
	ParamListInfo paramInfo = econtext->ecxt_param_list_info;

	if (isDone)
		*isDone = ExprSingleResult;

	/*
	 * PARAM_EXTERN parameters must be sought in ecxt_param_list_info.
	 */
	if (paramInfo &&
		thisParamId > 0 && thisParamId <= paramInfo->numParams)
	{
		ParamExternData *prm = &paramInfo->params[thisParamId - 1];

		/* give hook a chance in case parameter is dynamic */
		if (!OidIsValid(prm->ptype) && paramInfo->paramFetch != NULL)
			(*paramInfo->paramFetch) (paramInfo, thisParamId);

		if (OidIsValid(prm->ptype))
		{
			/* safety check in case hook did something unexpected */
			if (prm->ptype != expression->paramtype)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("type of parameter %d (%s) does not match that when preparing the plan (%s)",
								thisParamId,
								format_type_be(prm->ptype),
								format_type_be(expression->paramtype))));

			*isNull = prm->isnull;
			return prm->value;
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("no value found for parameter %d", thisParamId)));
	return (Datum) 0;			/* keep compiler quiet */
}


/* ----------------------------------------------------------------
 *		ExecEvalOper / ExecEvalFunc support routines
 * ----------------------------------------------------------------
 */

/*
 *		GetAttributeByName
 *		GetAttributeByNum
 *
 *		These functions return the value of the requested attribute
 *		out of the given tuple Datum.
 *		C functions which take a tuple as an argument are expected
 *		to use these.  Ex: overpaid(EMP) might call GetAttributeByNum().
 *		Note: these are actually rather slow because they do a typcache
 *		lookup on each call.
 */
Datum
GetAttributeByNum(HeapTupleHeader tuple,
				  AttrNumber attrno,
				  bool *isNull)
{
	Datum		result;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupDesc;
	HeapTupleData tmptup;

	if (!AttributeNumberIsValid(attrno))
		elog(ERROR, "invalid attribute number %d", attrno);

	if (isNull == NULL)
		elog(ERROR, "a NULL isNull pointer was passed");

	if (tuple == NULL)
	{
		/* Kinda bogus but compatible with old behavior... */
		*isNull = true;
		return (Datum) 0;
	}

	tupType = HeapTupleHeaderGetTypeId(tuple);
	tupTypmod = HeapTupleHeaderGetTypMod(tuple);
	tupDesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

	/*
	 * heap_getattr needs a HeapTuple not a bare HeapTupleHeader.  We set all
	 * the fields in the struct just in case user tries to inspect system
	 * columns.
	 */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = tuple;

	result = heap_getattr(&tmptup,
						  attrno,
						  tupDesc,
						  isNull);

	ReleaseTupleDesc(tupDesc);

	return result;
}

Datum
GetAttributeByName(HeapTupleHeader tuple, const char *attname, bool *isNull)
{
	AttrNumber	attrno;
	Datum		result;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupDesc;
	HeapTupleData tmptup;
	int			i;

	if (attname == NULL)
		elog(ERROR, "invalid attribute name");

	if (isNull == NULL)
		elog(ERROR, "a NULL isNull pointer was passed");

	if (tuple == NULL)
	{
		/* Kinda bogus but compatible with old behavior... */
		*isNull = true;
		return (Datum) 0;
	}

	tupType = HeapTupleHeaderGetTypeId(tuple);
	tupTypmod = HeapTupleHeaderGetTypMod(tuple);
	tupDesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

	attrno = InvalidAttrNumber;
	for (i = 0; i < tupDesc->natts; i++)
	{
		if (namestrcmp(&(tupDesc->attrs[i]->attname), attname) == 0)
		{
			attrno = tupDesc->attrs[i]->attnum;
			break;
		}
	}

	if (attrno == InvalidAttrNumber)
		elog(ERROR, "attribute \"%s\" does not exist", attname);

	/*
	 * heap_getattr needs a HeapTuple not a bare HeapTupleHeader.  We set all
	 * the fields in the struct just in case user tries to inspect system
	 * columns.
	 */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = tuple;

	result = heap_getattr(&tmptup,
						  attrno,
						  tupDesc,
						  isNull);

	ReleaseTupleDesc(tupDesc);

	return result;
}

/*
 * init_fcache - initialize a FuncExprState node during first use
 */
static void
init_fcache(Oid foid, Oid input_collation, FuncExprState *fcache,
			MemoryContext fcacheCxt, bool needDescForSets)
{
	AclResult	aclresult;

	/* Check permission to call function */
	aclresult = pg_proc_aclcheck(foid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_PROC, get_func_name(foid));
	InvokeFunctionExecuteHook(foid);

	/*
	 * Safety check on nargs.  Under normal circumstances this should never
	 * fail, as parser should check sooner.  But possibly it might fail if
	 * server has been compiled with FUNC_MAX_ARGS smaller than some functions
	 * declared in pg_proc?
	 */
	if (list_length(fcache->args) > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
			 errmsg_plural("cannot pass more than %d argument to a function",
						   "cannot pass more than %d arguments to a function",
						   FUNC_MAX_ARGS,
						   FUNC_MAX_ARGS)));

	/* Set up the primary fmgr lookup information */
	fmgr_info_cxt(foid, &(fcache->func), fcacheCxt);
	fmgr_info_set_expr((Node *) fcache->xprstate.expr, &(fcache->func));

	/* Initialize the function call parameter struct as well */
	InitFunctionCallInfoData(fcache->fcinfo_data, &(fcache->func),
							 list_length(fcache->args),
							 input_collation, NULL, NULL);

	/* If function returns set, prepare expected tuple descriptor */
	if (fcache->func.fn_retset && needDescForSets)
	{
		TypeFuncClass functypclass;
		Oid			funcrettype;
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		functypclass = get_expr_result_type(fcache->func.fn_expr,
											&funcrettype,
											&tupdesc);

		/* Must save tupdesc in fcache's context */
		oldcontext = MemoryContextSwitchTo(fcacheCxt);

		if (functypclass == TYPEFUNC_COMPOSITE)
		{
			/* Composite data type, e.g. a table's row type */
			Assert(tupdesc);
			/* Must copy it out of typcache for safety */
			fcache->funcResultDesc = CreateTupleDescCopy(tupdesc);
			fcache->funcReturnsTuple = true;
		}
		else if (functypclass == TYPEFUNC_SCALAR)
		{
			/* Base data type, i.e. scalar */
			tupdesc = CreateTemplateTupleDesc(1, false);
			TupleDescInitEntry(tupdesc,
							   (AttrNumber) 1,
							   NULL,
							   funcrettype,
							   -1,
							   0);
			fcache->funcResultDesc = tupdesc;
			fcache->funcReturnsTuple = false;
		}
		else if (functypclass == TYPEFUNC_RECORD)
		{
			/* This will work if function doesn't need an expectedDesc */
			fcache->funcResultDesc = NULL;
			fcache->funcReturnsTuple = true;
		}
		else
		{
			/* Else, we will fail if function needs an expectedDesc */
			fcache->funcResultDesc = NULL;
		}

		MemoryContextSwitchTo(oldcontext);
	}
	else
		fcache->funcResultDesc = NULL;

	/* Initialize additional state */
	fcache->funcResultStore = NULL;
	fcache->funcResultSlot = NULL;
	fcache->setArgsValid = false;
	fcache->shutdown_reg = false;
}

/*
 * callback function in case a FuncExpr returning a set needs to be shut down
 * before it has been run to completion
 */
static void
ShutdownFuncExpr(Datum arg)
{
	FuncExprState *fcache = (FuncExprState *) DatumGetPointer(arg);

	/* If we have a slot, make sure it's let go of any tuplestore pointer */
	if (fcache->funcResultSlot)
		ExecClearTuple(fcache->funcResultSlot);

	/* Release any open tuplestore */
	if (fcache->funcResultStore)
		tuplestore_end(fcache->funcResultStore);
	fcache->funcResultStore = NULL;

	/* Clear any active set-argument state */
	fcache->setArgsValid = false;

	/* execUtils will deregister the callback... */
	fcache->shutdown_reg = false;
}

/*
 * get_cached_rowtype: utility function to lookup a rowtype tupdesc
 *
 * type_id, typmod: identity of the rowtype
 * cache_field: where to cache the TupleDesc pointer in expression state node
 *		(field must be initialized to NULL)
 * econtext: expression context we are executing in
 *
 * NOTE: because the shutdown callback will be called during plan rescan,
 * must be prepared to re-do this during any node execution; cannot call
 * just once during expression initialization
 */
static TupleDesc
get_cached_rowtype(Oid type_id, int32 typmod,
				   TupleDesc *cache_field, ExprContext *econtext)
{
	TupleDesc	tupDesc = *cache_field;

	/* Do lookup if no cached value or if requested type changed */
	if (tupDesc == NULL ||
		type_id != tupDesc->tdtypeid ||
		typmod != tupDesc->tdtypmod)
	{
		tupDesc = lookup_rowtype_tupdesc(type_id, typmod);

		if (*cache_field)
		{
			/* Release old tupdesc; but callback is already registered */
			ReleaseTupleDesc(*cache_field);
		}
		else
		{
			/* Need to register shutdown callback to release tupdesc */
			RegisterExprContextCallback(econtext,
										ShutdownTupleDescRef,
										PointerGetDatum(cache_field));
		}
		*cache_field = tupDesc;
	}
	return tupDesc;
}

/*
 * Callback function to release a tupdesc refcount at expression tree shutdown
 */
static void
ShutdownTupleDescRef(Datum arg)
{
	TupleDesc  *cache_field = (TupleDesc *) DatumGetPointer(arg);

	if (*cache_field)
		ReleaseTupleDesc(*cache_field);
	*cache_field = NULL;
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
	ListCell   *arg;

	argIsDone = ExprSingleResult;		/* default assumption */

	i = 0;
	foreach(arg, argList)
	{
		ExprState  *argstate = (ExprState *) lfirst(arg);
		ExprDoneCond thisArgIsDone;

		fcinfo->arg[i] = ExecEvalExpr(argstate,
									  econtext,
									  &fcinfo->argnull[i],
									  &thisArgIsDone);

		if (thisArgIsDone != ExprSingleResult)
		{
			/*
			 * We allow only one argument to have a set value; we'd need much
			 * more complexity to keep track of multiple set arguments (cf.
			 * ExecTargetList) and it doesn't seem worth it.
			 */
			if (argIsDone != ExprSingleResult)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("functions and operators can take at most one set argument")));
			argIsDone = thisArgIsDone;
		}
		i++;
	}

	Assert(i == fcinfo->nargs);

	return argIsDone;
}

/*
 *		ExecPrepareTuplestoreResult
 *
 * Subroutine for ExecMakeFunctionResult: prepare to extract rows from a
 * tuplestore function result.	We must set up a funcResultSlot (unless
 * already done in a previous call cycle) and verify that the function
 * returned the expected tuple descriptor.
 */
static void
ExecPrepareTuplestoreResult(FuncExprState *fcache,
							ExprContext *econtext,
							Tuplestorestate *resultStore,
							TupleDesc resultDesc)
{
	fcache->funcResultStore = resultStore;

	if (fcache->funcResultSlot == NULL)
	{
		/* Create a slot so we can read data out of the tuplestore */
		TupleDesc	slotDesc;
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(fcache->func.fn_mcxt);

		/*
		 * If we were not able to determine the result rowtype from context,
		 * and the function didn't return a tupdesc, we have to fail.
		 */
		if (fcache->funcResultDesc)
			slotDesc = fcache->funcResultDesc;
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

		fcache->funcResultSlot = MakeSingleTupleTableSlot(slotDesc);
		MemoryContextSwitchTo(oldcontext);
	}

	/*
	 * If function provided a tupdesc, cross-check it.	We only really need to
	 * do this for functions returning RECORD, but might as well do it always.
	 */
	if (resultDesc)
	{
		if (fcache->funcResultDesc)
			tupledesc_match(fcache->funcResultDesc, resultDesc);

		/*
		 * If it is a dynamically-allocated TupleDesc, free it: it is
		 * typically allocated in a per-query context, so we must avoid
		 * leaking it across multiple usages.
		 */
		if (resultDesc->tdrefcount == -1)
			FreeTupleDesc(resultDesc);
	}

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
		Form_pg_attribute dattr = dst_tupdesc->attrs[i];
		Form_pg_attribute sattr = src_tupdesc->attrs[i];

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

/*
 *		ExecMakeFunctionResult
 *
 * Evaluate the arguments to a function and then the function itself.
 * init_fcache is presumed already run on the FuncExprState.
 *
 * This function handles the most general case, wherein the function or
 * one of its arguments might (or might not) return a set.	If we find
 * no sets involved, we will change the FuncExprState's function pointer
 * to use a simpler method on subsequent calls.
 */
static Datum
ExecMakeFunctionResult(FuncExprState *fcache,
					   ExprContext *econtext,
					   bool *isNull,
					   ExprDoneCond *isDone)
{
	List	   *arguments;
	Datum		result;
	FunctionCallInfo fcinfo;
	PgStat_FunctionCallUsage fcusage;
	ReturnSetInfo rsinfo;		/* for functions returning sets */
	ExprDoneCond argDone;
	bool		hasSetArg;
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
		Assert(isDone);			/* it was provided before ... */
		if (tuplestore_gettupleslot(fcache->funcResultStore, true, false,
									fcache->funcResultSlot))
		{
			*isDone = ExprMultipleResult;
			if (fcache->funcReturnsTuple)
			{
				/* We must return the whole tuple as a Datum. */
				*isNull = false;
				return ExecFetchSlotTupleDatum(fcache->funcResultSlot);
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
		/* We are done unless there was a set-valued argument */
		if (!fcache->setHasSetArg)
		{
			*isDone = ExprEndResult;
			*isNull = true;
			return (Datum) 0;
		}
		/* If there was, continue evaluating the argument values */
		Assert(!fcache->setArgsValid);
	}

	/*
	 * arguments is a list of expressions to evaluate before passing to the
	 * function manager.  We skip the evaluation if it was already done in the
	 * previous call (ie, we are continuing the evaluation of a set-valued
	 * function).  Otherwise, collect the current argument values into fcinfo.
	 */
	fcinfo = &fcache->fcinfo_data;
	arguments = fcache->args;
	if (!fcache->setArgsValid)
	{
		argDone = ExecEvalFuncArgs(fcinfo, arguments, econtext);
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
		/* Re-use callinfo from previous evaluation */
		hasSetArg = fcache->setHasSetArg;
		/* Reset flag (we may set it again below) */
		fcache->setArgsValid = false;
	}

	/*
	 * Now call the function, passing the evaluated parameter values.
	 */
	if (fcache->func.fn_retset || hasSetArg)
	{
		/*
		 * We need to return a set result.	Complain if caller not ready to
		 * accept one.
		 */
		if (isDone == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("set-valued function called in context that cannot accept a set")));

		/*
		 * Prepare a resultinfo node for communication.  If the function
		 * doesn't itself return set, we don't pass the resultinfo to the
		 * function, but we need to fill it in anyway for internal use.
		 */
		if (fcache->func.fn_retset)
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
		 * This loop handles the situation where we have both a set argument
		 * and a set-valued function.  Once we have exhausted the function's
		 * value(s) for a particular argument value, we have to get the next
		 * argument value and start the function over again. We might have to
		 * do it more than once, if the function produces an empty result set
		 * for a particular input value.
		 */
		for (;;)
		{
			/*
			 * If function is strict, and there are any NULL arguments, skip
			 * calling the function (at least for this set of args).
			 */
			bool		callit = true;

			if (fcache->func.fn_strict)
			{
				for (i = 0; i < fcinfo->nargs; i++)
				{
					if (fcinfo->argnull[i])
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
			else if (fcache->func.fn_retset)
			{
				/* for a strict SRF, result for NULL is an empty set */
				result = (Datum) 0;
				*isNull = true;
				*isDone = ExprEndResult;
			}
			else
			{
				/* for a strict non-SRF, result for NULL is a NULL */
				result = (Datum) 0;
				*isNull = true;
				*isDone = ExprSingleResult;
			}

			/* Which protocol does function want to use? */
			if (rsinfo.returnMode == SFRM_ValuePerCall)
			{
				if (*isDone != ExprEndResult)
				{
					/*
					 * Got a result from current argument. If function itself
					 * returns set, save the current argument values to re-use
					 * on the next call.
					 */
					if (fcache->func.fn_retset &&
						*isDone == ExprMultipleResult)
					{
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
					if (hasSetArg)
						*isDone = ExprMultipleResult;
					break;
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
					/* remember whether we had set arguments */
					fcache->setHasSetArg = hasSetArg;
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

			/* Else, done with this argument */
			if (!hasSetArg)
				break;			/* input not a set, so done */

			/* Re-eval args to get the next element of the input set */
			argDone = ExecEvalFuncArgs(fcinfo, arguments, econtext);

			if (argDone != ExprMultipleResult)
			{
				/* End of argument set, so we're done. */
				*isNull = true;
				*isDone = ExprEndResult;
				result = (Datum) 0;
				break;
			}

			/*
			 * If we reach here, loop around to run the function on the new
			 * argument.
			 */
		}
	}
	else
	{
		/*
		 * Non-set case: much easier.
		 *
		 * We change the ExprState function pointer to use the simpler
		 * ExecMakeFunctionResultNoSets on subsequent calls.  This amounts to
		 * assuming that no argument can return a set if it didn't do so the
		 * first time.
		 */
		fcache->xprstate.evalfunc = (ExprStateEvalFunc) ExecMakeFunctionResultNoSets;

		if (isDone)
			*isDone = ExprSingleResult;

		/*
		 * If function is strict, and there are any NULL arguments, skip
		 * calling the function and return NULL.
		 */
		if (fcache->func.fn_strict)
		{
			for (i = 0; i < fcinfo->nargs; i++)
			{
				if (fcinfo->argnull[i])
				{
					*isNull = true;
					return (Datum) 0;
				}
			}
		}

		pgstat_init_function_usage(fcinfo, &fcusage);

		fcinfo->isnull = false;
		result = FunctionCallInvoke(fcinfo);
		*isNull = fcinfo->isnull;

		pgstat_end_function_usage(&fcusage, true);
	}

	return result;
}

/*
 *		ExecMakeFunctionResultNoSets
 *
 * Simplified version of ExecMakeFunctionResult that can only handle
 * non-set cases.  Hand-tuned for speed.
 */
static Datum
ExecMakeFunctionResultNoSets(FuncExprState *fcache,
							 ExprContext *econtext,
							 bool *isNull,
							 ExprDoneCond *isDone)
{
	ListCell   *arg;
	Datum		result;
	FunctionCallInfo fcinfo;
	PgStat_FunctionCallUsage fcusage;
	int			i;

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	if (isDone)
		*isDone = ExprSingleResult;

	/* inlined, simplified version of ExecEvalFuncArgs */
	fcinfo = &fcache->fcinfo_data;
	i = 0;
	foreach(arg, fcache->args)
	{
		ExprState  *argstate = (ExprState *) lfirst(arg);

		fcinfo->arg[i] = ExecEvalExpr(argstate,
									  econtext,
									  &fcinfo->argnull[i],
									  NULL);
		i++;
	}

	/*
	 * If function is strict, and there are any NULL arguments, skip calling
	 * the function and return NULL.
	 */
	if (fcache->func.fn_strict)
	{
		while (--i >= 0)
		{
			if (fcinfo->argnull[i])
			{
				*isNull = true;
				return (Datum) 0;
			}
		}
	}

	pgstat_init_function_usage(fcinfo, &fcusage);

	fcinfo->isnull = false;
	result = FunctionCallInvoke(fcinfo);
	*isNull = fcinfo->isnull;

	pgstat_end_function_usage(&fcusage, true);

	return result;
}


/*
 *		ExecMakeTableFunctionResult
 *
 * Evaluate a table function, producing a materialized result in a Tuplestore
 * object.
 */
Tuplestorestate *
ExecMakeTableFunctionResult(ExprState *funcexpr,
							ExprContext *econtext,
							TupleDesc expectedDesc,
							bool randomAccess)
{
	Tuplestorestate *tupstore = NULL;
	TupleDesc	tupdesc = NULL;
	Oid			funcrettype;
	bool		returnsTuple;
	bool		returnsSet = false;
	FunctionCallInfoData fcinfo;
	PgStat_FunctionCallUsage fcusage;
	ReturnSetInfo rsinfo;
	HeapTupleData tmptup;
	MemoryContext callerContext;
	MemoryContext oldcontext;
	bool		direct_function_call;
	bool		first_time = true;

	callerContext = CurrentMemoryContext;

	funcrettype = exprType((Node *) funcexpr->expr);

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

	/*
	 * Normally the passed expression tree will be a FuncExprState, since the
	 * grammar only allows a function call at the top level of a table
	 * function reference.	However, if the function doesn't return set then
	 * the planner might have replaced the function call via constant-folding
	 * or inlining.  So if we see any other kind of expression node, execute
	 * it via the general ExecEvalExpr() code; the only difference is that we
	 * don't get a chance to pass a special ReturnSetInfo to any functions
	 * buried in the expression.
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

			init_fcache(func->funcid, func->inputcollid, fcache,
						econtext->ecxt_per_query_memory, false);
		}
		returnsSet = fcache->func.fn_retset;
		InitFunctionCallInfoData(fcinfo, &(fcache->func),
								 list_length(fcache->args),
								 fcache->fcinfo_data.fncollation,
								 NULL, (Node *) &rsinfo);

		/*
		 * Evaluate the function's argument list.
		 *
		 * Note: ideally, we'd do this in the per-tuple context, but then the
		 * argument values would disappear when we reset the context in the
		 * inner loop.	So do it in caller context.  Perhaps we should make a
		 * separate context just to hold the evaluated arguments?
		 */
		argDone = ExecEvalFuncArgs(&fcinfo, fcache->args, econtext);
		/* We don't allow sets in the arguments of the table function */
		if (argDone != ExprSingleResult)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("set-valued function called in context that cannot accept a set")));

		/*
		 * If function is strict, and there are any NULL arguments, skip
		 * calling the function and act like it returned NULL (or an empty
		 * set, in the returns-set case).
		 */
		if (fcache->func.fn_strict)
		{
			int			i;

			for (i = 0; i < fcinfo.nargs; i++)
			{
				if (fcinfo.argnull[i])
					goto no_function_result;
			}
		}
	}
	else
	{
		/* Treat funcexpr as a generic expression */
		direct_function_call = false;
		InitFunctionCallInfoData(fcinfo, NULL, 0, InvalidOid, NULL, NULL);
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
		 * reset per-tuple memory context before each call of the function or
		 * expression. This cleans up any local memory the function may leak
		 * when called.
		 */
		ResetExprContext(econtext);

		/* Call the function or expression one time */
		if (direct_function_call)
		{
			pgstat_init_function_usage(&fcinfo, &fcusage);

			fcinfo.isnull = false;
			rsinfo.isDone = ExprSingleResult;
			result = FunctionCallInvoke(&fcinfo);

			pgstat_end_function_usage(&fcusage,
									  rsinfo.isDone != ExprMultipleResult);
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
			 */
			if (rsinfo.isDone == ExprEndResult)
				break;

			/*
			 * Can't do anything very useful with NULL rowtype values. For a
			 * function returning set, we consider this a protocol violation
			 * (but another alternative would be to just ignore the result and
			 * "continue" to get another row).	For a function not returning
			 * set, we fall out of the loop; we'll cons up an all-nulls result
			 * row below.
			 */
			if (returnsTuple && fcinfo.isnull)
			{
				if (!returnsSet)
					break;
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						 errmsg("function returning set of rows cannot return null value")));
			}

			/*
			 * If first time through, build tupdesc and tuplestore for result
			 */
			if (first_time)
			{
				oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
				if (returnsTuple)
				{
					/*
					 * Use the type info embedded in the rowtype Datum to look
					 * up the needed tupdesc.  Make a copy for the query.
					 */
					HeapTupleHeader td;

					td = DatumGetHeapTupleHeader(result);
					tupdesc = lookup_rowtype_tupdesc_copy(HeapTupleHeaderGetTypeId(td),
											   HeapTupleHeaderGetTypMod(td));
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
									   0);
				}
				tupstore = tuplestore_begin_heap(randomAccess, false, work_mem);
				MemoryContextSwitchTo(oldcontext);
				rsinfo.setResult = tupstore;
				rsinfo.setDesc = tupdesc;
			}

			/*
			 * Store current resultset item.
			 */
			if (returnsTuple)
			{
				HeapTupleHeader td;

				td = DatumGetHeapTupleHeader(result);

				/*
				 * Verify all returned rows have same subtype; necessary in
				 * case the type is RECORD.
				 */
				if (HeapTupleHeaderGetTypeId(td) != tupdesc->tdtypeid ||
					HeapTupleHeaderGetTypMod(td) != tupdesc->tdtypmod)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("rows returned by function are not all of the same row type")));

				/*
				 * tuplestore_puttuple needs a HeapTuple not a bare
				 * HeapTupleHeader, but it doesn't need all the fields.
				 */
				tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
				tmptup.t_data = td;

				tuplestore_puttuple(tupstore, &tmptup);
			}
			else
				tuplestore_putvalues(tupstore, tupdesc, &result, &fcinfo.isnull);

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
	 * non-set-returning function then insert a single all-nulls row.
	 */
	if (rsinfo.setResult == NULL)
	{
		MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
		tupstore = tuplestore_begin_heap(randomAccess, false, work_mem);
		rsinfo.setResult = tupstore;
		if (!returnsSet)
		{
			int			natts = expectedDesc->natts;
			Datum	   *nulldatums;
			bool	   *nullflags;

			MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
			nulldatums = (Datum *) palloc0(natts * sizeof(Datum));
			nullflags = (bool *) palloc(natts * sizeof(bool));
			memset(nullflags, true, natts * sizeof(bool));
			MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
			tuplestore_putvalues(tupstore, expectedDesc, nulldatums, nullflags);
		}
	}

	/*
	 * If function provided a tupdesc, cross-check it.	We only really need to
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
	/* This is called only the first time through */
	FuncExpr   *func = (FuncExpr *) fcache->xprstate.expr;

	/* Initialize function lookup info */
	init_fcache(func->funcid, func->inputcollid, fcache,
				econtext->ecxt_per_query_memory, true);

	/* Go directly to ExecMakeFunctionResult on subsequent uses */
	fcache->xprstate.evalfunc = (ExprStateEvalFunc) ExecMakeFunctionResult;

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
	/* This is called only the first time through */
	OpExpr	   *op = (OpExpr *) fcache->xprstate.expr;

	/* Initialize function lookup info */
	init_fcache(op->opfuncid, op->inputcollid, fcache,
				econtext->ecxt_per_query_memory, true);

	/* Go directly to ExecMakeFunctionResult on subsequent uses */
	fcache->xprstate.evalfunc = (ExprStateEvalFunc) ExecMakeFunctionResult;

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
				 bool *isNull,
				 ExprDoneCond *isDone)
{
	Datum		result;
	FunctionCallInfo fcinfo;
	ExprDoneCond argDone;

	/* Set default values for result flags: non-null, not a set result */
	*isNull = false;
	if (isDone)
		*isDone = ExprSingleResult;

	/*
	 * Initialize function cache if first time through
	 */
	if (fcache->func.fn_oid == InvalidOid)
	{
		DistinctExpr *op = (DistinctExpr *) fcache->xprstate.expr;

		init_fcache(op->opfuncid, op->inputcollid, fcache,
					econtext->ecxt_per_query_memory, true);
		Assert(!fcache->func.fn_retset);
	}

	/*
	 * Evaluate arguments
	 */
	fcinfo = &fcache->fcinfo_data;
	argDone = ExecEvalFuncArgs(fcinfo, fcache->args, econtext);
	if (argDone != ExprSingleResult)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("IS DISTINCT FROM does not support set arguments")));
	Assert(fcinfo->nargs == 2);

	if (fcinfo->argnull[0] && fcinfo->argnull[1])
	{
		/* Both NULL? Then is not distinct... */
		result = BoolGetDatum(FALSE);
	}
	else if (fcinfo->argnull[0] || fcinfo->argnull[1])
	{
		/* Only one is NULL? Then is distinct... */
		result = BoolGetDatum(TRUE);
	}
	else
	{
		fcinfo->isnull = false;
		result = FunctionCallInvoke(fcinfo);
		*isNull = fcinfo->isnull;
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
					  ExprContext *econtext,
					  bool *isNull, ExprDoneCond *isDone)
{
	ScalarArrayOpExpr *opexpr = (ScalarArrayOpExpr *) sstate->fxprstate.xprstate.expr;
	bool		useOr = opexpr->useOr;
	ArrayType  *arr;
	int			nitems;
	Datum		result;
	bool		resultnull;
	FunctionCallInfo fcinfo;
	ExprDoneCond argDone;
	int			i;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char	   *s;
	bits8	   *bitmap;
	int			bitmask;

	/* Set default values for result flags: non-null, not a set result */
	*isNull = false;
	if (isDone)
		*isDone = ExprSingleResult;

	/*
	 * Initialize function cache if first time through
	 */
	if (sstate->fxprstate.func.fn_oid == InvalidOid)
	{
		init_fcache(opexpr->opfuncid, opexpr->inputcollid, &sstate->fxprstate,
					econtext->ecxt_per_query_memory, true);
		Assert(!sstate->fxprstate.func.fn_retset);
	}

	/*
	 * Evaluate arguments
	 */
	fcinfo = &sstate->fxprstate.fcinfo_data;
	argDone = ExecEvalFuncArgs(fcinfo, sstate->fxprstate.args, econtext);
	if (argDone != ExprSingleResult)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
			   errmsg("op ANY/ALL (array) does not support set arguments")));
	Assert(fcinfo->nargs == 2);

	/*
	 * If the array is NULL then we return NULL --- it's not very meaningful
	 * to do anything else, even if the operator isn't strict.
	 */
	if (fcinfo->argnull[1])
	{
		*isNull = true;
		return (Datum) 0;
	}
	/* Else okay to fetch and detoast the array */
	arr = DatumGetArrayTypeP(fcinfo->arg[1]);

	/*
	 * If the array is empty, we return either FALSE or TRUE per the useOr
	 * flag.  This is correct even if the scalar is NULL; since we would
	 * evaluate the operator zero times, it matters not whether it would want
	 * to return NULL.
	 */
	nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
	if (nitems <= 0)
		return BoolGetDatum(!useOr);

	/*
	 * If the scalar is NULL, and the function is strict, return NULL; no
	 * point in iterating the loop.
	 */
	if (fcinfo->argnull[0] && sstate->fxprstate.func.fn_strict)
	{
		*isNull = true;
		return (Datum) 0;
	}

	/*
	 * We arrange to look up info about the element type only once per series
	 * of calls, assuming the element type doesn't change underneath us.
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
	bitmap = ARR_NULLBITMAP(arr);
	bitmask = 1;

	for (i = 0; i < nitems; i++)
	{
		Datum		elt;
		Datum		thisresult;

		/* Get array element, checking for NULL */
		if (bitmap && (*bitmap & bitmask) == 0)
		{
			fcinfo->arg[1] = (Datum) 0;
			fcinfo->argnull[1] = true;
		}
		else
		{
			elt = fetch_att(s, typbyval, typlen);
			s = att_addlength_pointer(s, typlen, s);
			s = (char *) att_align_nominal(s, typalign);
			fcinfo->arg[1] = elt;
			fcinfo->argnull[1] = false;
		}

		/* Call comparison function */
		if (fcinfo->argnull[1] && sstate->fxprstate.func.fn_strict)
		{
			fcinfo->isnull = true;
			thisresult = (Datum) 0;
		}
		else
		{
			fcinfo->isnull = false;
			thisresult = FunctionCallInvoke(fcinfo);
		}

		/* Combine results per OR or AND semantics */
		if (fcinfo->isnull)
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

		/* advance bitmap pointer if any */
		if (bitmap)
		{
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				bitmap++;
				bitmask = 1;
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
ExecEvalNot(BoolExprState *notclause, ExprContext *econtext,
			bool *isNull, ExprDoneCond *isDone)
{
	ExprState  *clause = linitial(notclause->args);
	Datum		expr_value;

	if (isDone)
		*isDone = ExprSingleResult;

	expr_value = ExecEvalExpr(clause, econtext, isNull, NULL);

	/*
	 * if the expression evaluates to null, then we just cascade the null back
	 * to whoever called us.
	 */
	if (*isNull)
		return expr_value;

	/*
	 * evaluation of 'not' is simple.. expr is false, then return 'true' and
	 * vice versa.
	 */
	return BoolGetDatum(!DatumGetBool(expr_value));
}

/* ----------------------------------------------------------------
 *		ExecEvalOr
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalOr(BoolExprState *orExpr, ExprContext *econtext,
		   bool *isNull, ExprDoneCond *isDone)
{
	List	   *clauses = orExpr->args;
	ListCell   *clause;
	bool		AnyNull;

	if (isDone)
		*isDone = ExprSingleResult;

	AnyNull = false;

	/*
	 * If any of the clauses is TRUE, the OR result is TRUE regardless of the
	 * states of the rest of the clauses, so we can stop evaluating and return
	 * TRUE immediately.  If none are TRUE and one or more is NULL, we return
	 * NULL; otherwise we return FALSE.  This makes sense when you interpret
	 * NULL as "don't know": if we have a TRUE then the OR is TRUE even if we
	 * aren't sure about some of the other inputs. If all the known inputs are
	 * FALSE, but we have one or more "don't knows", then we have to report
	 * that we "don't know" what the OR's result should be --- perhaps one of
	 * the "don't knows" would have been TRUE if we'd known its value.  Only
	 * when all the inputs are known to be FALSE can we state confidently that
	 * the OR's result is FALSE.
	 */
	foreach(clause, clauses)
	{
		ExprState  *clausestate = (ExprState *) lfirst(clause);
		Datum		clause_value;

		clause_value = ExecEvalExpr(clausestate, econtext, isNull, NULL);

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
ExecEvalAnd(BoolExprState *andExpr, ExprContext *econtext,
			bool *isNull, ExprDoneCond *isDone)
{
	List	   *clauses = andExpr->args;
	ListCell   *clause;
	bool		AnyNull;

	if (isDone)
		*isDone = ExprSingleResult;

	AnyNull = false;

	/*
	 * If any of the clauses is FALSE, the AND result is FALSE regardless of
	 * the states of the rest of the clauses, so we can stop evaluating and
	 * return FALSE immediately.  If none are FALSE and one or more is NULL,
	 * we return NULL; otherwise we return TRUE.  This makes sense when you
	 * interpret NULL as "don't know", using the same sort of reasoning as for
	 * OR, above.
	 */

	foreach(clause, clauses)
	{
		ExprState  *clausestate = (ExprState *) lfirst(clause);
		Datum		clause_value;

		clause_value = ExecEvalExpr(clausestate, econtext, isNull, NULL);

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
 *		ExecEvalConvertRowtype
 *
 *		Evaluate a rowtype coercion operation.	This may require
 *		rearranging field positions.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalConvertRowtype(ConvertRowtypeExprState *cstate,
					   ExprContext *econtext,
					   bool *isNull, ExprDoneCond *isDone)
{
	ConvertRowtypeExpr *convert = (ConvertRowtypeExpr *) cstate->xprstate.expr;
	HeapTuple	result;
	Datum		tupDatum;
	HeapTupleHeader tuple;
	HeapTupleData tmptup;

	tupDatum = ExecEvalExpr(cstate->arg, econtext, isNull, isDone);

	/* this test covers the isDone exception too: */
	if (*isNull)
		return tupDatum;

	tuple = DatumGetHeapTupleHeader(tupDatum);

	/* Lookup tupdescs if first time through or after rescan */
	if (cstate->indesc == NULL)
	{
		get_cached_rowtype(exprType((Node *) convert->arg), -1,
						   &cstate->indesc, econtext);
		cstate->initialized = false;
	}
	if (cstate->outdesc == NULL)
	{
		get_cached_rowtype(convert->resulttype, -1,
						   &cstate->outdesc, econtext);
		cstate->initialized = false;
	}

	Assert(HeapTupleHeaderGetTypeId(tuple) == cstate->indesc->tdtypeid);
	Assert(HeapTupleHeaderGetTypMod(tuple) == cstate->indesc->tdtypmod);

	/* if first time through, initialize conversion map */
	if (!cstate->initialized)
	{
		MemoryContext old_cxt;

		/* allocate map in long-lived memory context */
		old_cxt = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);

		/* prepare map from old to new attribute numbers */
		cstate->map = convert_tuples_by_name(cstate->indesc,
											 cstate->outdesc,
								 gettext_noop("could not convert row type"));
		cstate->initialized = true;

		MemoryContextSwitchTo(old_cxt);
	}

	/*
	 * No-op if no conversion needed (not clear this can happen here).
	 */
	if (cstate->map == NULL)
		return tupDatum;

	/*
	 * do_convert_tuple needs a HeapTuple not a bare HeapTupleHeader.
	 */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
	tmptup.t_data = tuple;

	result = do_convert_tuple(&tmptup, cstate->map);

	return HeapTupleGetDatum(result);
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
	List	   *clauses = caseExpr->args;
	ListCell   *clause;
	Datum		save_datum;
	bool		save_isNull;

	if (isDone)
		*isDone = ExprSingleResult;

	/*
	 * If there's a test expression, we have to evaluate it and save the value
	 * where the CaseTestExpr placeholders can find it. We must save and
	 * restore prior setting of econtext's caseValue fields, in case this node
	 * is itself within a larger CASE.
	 */
	save_datum = econtext->caseValue_datum;
	save_isNull = econtext->caseValue_isNull;

	if (caseExpr->arg)
	{
		econtext->caseValue_datum = ExecEvalExpr(caseExpr->arg,
												 econtext,
												 &econtext->caseValue_isNull,
												 NULL);
	}

	/*
	 * we evaluate each of the WHEN clauses in turn, as soon as one is true we
	 * return the corresponding result. If none are true then we return the
	 * value of the default clause, or NULL if there is none.
	 */
	foreach(clause, clauses)
	{
		CaseWhenState *wclause = lfirst(clause);
		Datum		clause_value;

		clause_value = ExecEvalExpr(wclause->expr,
									econtext,
									isNull,
									NULL);

		/*
		 * if we have a true test, then we return the result, since the case
		 * statement is satisfied.	A NULL result from the test is not
		 * considered true.
		 */
		if (DatumGetBool(clause_value) && !*isNull)
		{
			econtext->caseValue_datum = save_datum;
			econtext->caseValue_isNull = save_isNull;
			return ExecEvalExpr(wclause->result,
								econtext,
								isNull,
								isDone);
		}
	}

	econtext->caseValue_datum = save_datum;
	econtext->caseValue_isNull = save_isNull;

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

/*
 * ExecEvalCaseTestExpr
 *
 * Return the value stored by CASE.
 */
static Datum
ExecEvalCaseTestExpr(ExprState *exprstate,
					 ExprContext *econtext,
					 bool *isNull, ExprDoneCond *isDone)
{
	if (isDone)
		*isDone = ExprSingleResult;
	*isNull = econtext->caseValue_isNull;
	return econtext->caseValue_datum;
}

/* ----------------------------------------------------------------
 *		ExecEvalArray - ARRAY[] expressions
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalArray(ArrayExprState *astate, ExprContext *econtext,
			  bool *isNull, ExprDoneCond *isDone)
{
	ArrayExpr  *arrayExpr = (ArrayExpr *) astate->xprstate.expr;
	ArrayType  *result;
	ListCell   *element;
	Oid			element_type = arrayExpr->element_typeid;
	int			ndims = 0;
	int			dims[MAXDIM];
	int			lbs[MAXDIM];

	/* Set default values for result flags: non-null, not a set result */
	*isNull = false;
	if (isDone)
		*isDone = ExprSingleResult;

	if (!arrayExpr->multidims)
	{
		/* Elements are presumably of scalar type */
		int			nelems;
		Datum	   *dvalues;
		bool	   *dnulls;
		int			i = 0;

		ndims = 1;
		nelems = list_length(astate->elements);

		/* Shouldn't happen here, but if length is 0, return empty array */
		if (nelems == 0)
			return PointerGetDatum(construct_empty_array(element_type));

		dvalues = (Datum *) palloc(nelems * sizeof(Datum));
		dnulls = (bool *) palloc(nelems * sizeof(bool));

		/* loop through and build array of datums */
		foreach(element, astate->elements)
		{
			ExprState  *e = (ExprState *) lfirst(element);

			dvalues[i] = ExecEvalExpr(e, econtext, &dnulls[i], NULL);
			i++;
		}

		/* setup for 1-D array of the given length */
		dims[0] = nelems;
		lbs[0] = 1;

		result = construct_md_array(dvalues, dnulls, ndims, dims, lbs,
									element_type,
									astate->elemlength,
									astate->elembyval,
									astate->elemalign);
	}
	else
	{
		/* Must be nested array expressions */
		int			nbytes = 0;
		int			nitems = 0;
		int			outer_nelems = 0;
		int			elem_ndims = 0;
		int		   *elem_dims = NULL;
		int		   *elem_lbs = NULL;
		bool		firstone = true;
		bool		havenulls = false;
		bool		haveempty = false;
		char	  **subdata;
		bits8	  **subbitmaps;
		int		   *subbytes;
		int		   *subnitems;
		int			i;
		int32		dataoffset;
		char	   *dat;
		int			iitem;

		i = list_length(astate->elements);
		subdata = (char **) palloc(i * sizeof(char *));
		subbitmaps = (bits8 **) palloc(i * sizeof(bits8 *));
		subbytes = (int *) palloc(i * sizeof(int));
		subnitems = (int *) palloc(i * sizeof(int));

		/* loop through and get data area from each element */
		foreach(element, astate->elements)
		{
			ExprState  *e = (ExprState *) lfirst(element);
			bool		eisnull;
			Datum		arraydatum;
			ArrayType  *array;
			int			this_ndims;

			arraydatum = ExecEvalExpr(e, econtext, &eisnull, NULL);
			/* temporarily ignore null subarrays */
			if (eisnull)
			{
				haveempty = true;
				continue;
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

			this_ndims = ARR_NDIM(array);
			/* temporarily ignore zero-dimensional subarrays */
			if (this_ndims <= 0)
			{
				haveempty = true;
				continue;
			}

			if (firstone)
			{
				/* Get sub-array details from first member */
				elem_ndims = this_ndims;
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
				if (elem_ndims != this_ndims ||
					memcmp(elem_dims, ARR_DIMS(array),
						   elem_ndims * sizeof(int)) != 0 ||
					memcmp(elem_lbs, ARR_LBOUND(array),
						   elem_ndims * sizeof(int)) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
							 errmsg("multidimensional arrays must have array "
									"expressions with matching dimensions")));
			}

			subdata[outer_nelems] = ARR_DATA_PTR(array);
			subbitmaps[outer_nelems] = ARR_NULLBITMAP(array);
			subbytes[outer_nelems] = ARR_SIZE(array) - ARR_DATA_OFFSET(array);
			nbytes += subbytes[outer_nelems];
			subnitems[outer_nelems] = ArrayGetNItems(this_ndims,
													 ARR_DIMS(array));
			nitems += subnitems[outer_nelems];
			havenulls |= ARR_HASNULL(array);
			outer_nelems++;
		}

		/*
		 * If all items were null or empty arrays, return an empty array;
		 * otherwise, if some were and some weren't, raise error.  (Note: we
		 * must special-case this somehow to avoid trying to generate a 1-D
		 * array formed from empty arrays.	It's not ideal...)
		 */
		if (haveempty)
		{
			if (ndims == 0)		/* didn't find any nonempty array */
				return PointerGetDatum(construct_empty_array(element_type));
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("multidimensional arrays must have array "
							"expressions with matching dimensions")));
		}

		/* setup for multi-D array */
		dims[0] = outer_nelems;
		lbs[0] = 1;
		for (i = 1; i < ndims; i++)
		{
			dims[i] = elem_dims[i - 1];
			lbs[i] = elem_lbs[i - 1];
		}

		if (havenulls)
		{
			dataoffset = ARR_OVERHEAD_WITHNULLS(ndims, nitems);
			nbytes += dataoffset;
		}
		else
		{
			dataoffset = 0;		/* marker for no null bitmap */
			nbytes += ARR_OVERHEAD_NONULLS(ndims);
		}

		result = (ArrayType *) palloc(nbytes);
		SET_VARSIZE(result, nbytes);
		result->ndim = ndims;
		result->dataoffset = dataoffset;
		result->elemtype = element_type;
		memcpy(ARR_DIMS(result), dims, ndims * sizeof(int));
		memcpy(ARR_LBOUND(result), lbs, ndims * sizeof(int));

		dat = ARR_DATA_PTR(result);
		iitem = 0;
		for (i = 0; i < outer_nelems; i++)
		{
			memcpy(dat, subdata[i], subbytes[i]);
			dat += subbytes[i];
			if (havenulls)
				array_bitmap_copy(ARR_NULLBITMAP(result), iitem,
								  subbitmaps[i], 0,
								  subnitems[i]);
			iitem += subnitems[i];
		}
	}

	return PointerGetDatum(result);
}

/* ----------------------------------------------------------------
 *		ExecEvalRow - ROW() expressions
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalRow(RowExprState *rstate,
			ExprContext *econtext,
			bool *isNull, ExprDoneCond *isDone)
{
	HeapTuple	tuple;
	Datum	   *values;
	bool	   *isnull;
	int			natts;
	ListCell   *arg;
	int			i;

	/* Set default values for result flags: non-null, not a set result */
	*isNull = false;
	if (isDone)
		*isDone = ExprSingleResult;

	/* Allocate workspace */
	natts = rstate->tupdesc->natts;
	values = (Datum *) palloc0(natts * sizeof(Datum));
	isnull = (bool *) palloc(natts * sizeof(bool));

	/* preset to nulls in case rowtype has some later-added columns */
	memset(isnull, true, natts * sizeof(bool));

	/* Evaluate field values */
	i = 0;
	foreach(arg, rstate->args)
	{
		ExprState  *e = (ExprState *) lfirst(arg);

		values[i] = ExecEvalExpr(e, econtext, &isnull[i], NULL);
		i++;
	}

	tuple = heap_form_tuple(rstate->tupdesc, values, isnull);

	pfree(values);
	pfree(isnull);

	return HeapTupleGetDatum(tuple);
}

/* ----------------------------------------------------------------
 *		ExecEvalRowCompare - ROW() comparison-op ROW()
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalRowCompare(RowCompareExprState *rstate,
				   ExprContext *econtext,
				   bool *isNull, ExprDoneCond *isDone)
{
	bool		result;
	RowCompareType rctype = ((RowCompareExpr *) rstate->xprstate.expr)->rctype;
	int32		cmpresult = 0;
	ListCell   *l;
	ListCell   *r;
	int			i;

	if (isDone)
		*isDone = ExprSingleResult;
	*isNull = true;				/* until we get a result */

	i = 0;
	forboth(l, rstate->largs, r, rstate->rargs)
	{
		ExprState  *le = (ExprState *) lfirst(l);
		ExprState  *re = (ExprState *) lfirst(r);
		FunctionCallInfoData locfcinfo;

		InitFunctionCallInfoData(locfcinfo, &(rstate->funcs[i]), 2,
								 rstate->collations[i],
								 NULL, NULL);
		locfcinfo.arg[0] = ExecEvalExpr(le, econtext,
										&locfcinfo.argnull[0], NULL);
		locfcinfo.arg[1] = ExecEvalExpr(re, econtext,
										&locfcinfo.argnull[1], NULL);
		if (rstate->funcs[i].fn_strict &&
			(locfcinfo.argnull[0] || locfcinfo.argnull[1]))
			return (Datum) 0;	/* force NULL result */
		locfcinfo.isnull = false;
		cmpresult = DatumGetInt32(FunctionCallInvoke(&locfcinfo));
		if (locfcinfo.isnull)
			return (Datum) 0;	/* force NULL result */
		if (cmpresult != 0)
			break;				/* no need to compare remaining columns */
		i++;
	}

	switch (rctype)
	{
			/* EQ and NE cases aren't allowed here */
		case ROWCOMPARE_LT:
			result = (cmpresult < 0);
			break;
		case ROWCOMPARE_LE:
			result = (cmpresult <= 0);
			break;
		case ROWCOMPARE_GE:
			result = (cmpresult >= 0);
			break;
		case ROWCOMPARE_GT:
			result = (cmpresult > 0);
			break;
		default:
			elog(ERROR, "unrecognized RowCompareType: %d", (int) rctype);
			result = 0;			/* keep compiler quiet */
			break;
	}

	*isNull = false;
	return BoolGetDatum(result);
}

/* ----------------------------------------------------------------
 *		ExecEvalCoalesce
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalCoalesce(CoalesceExprState *coalesceExpr, ExprContext *econtext,
				 bool *isNull, ExprDoneCond *isDone)
{
	ListCell   *arg;

	if (isDone)
		*isDone = ExprSingleResult;

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
 *		ExecEvalMinMax
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalMinMax(MinMaxExprState *minmaxExpr, ExprContext *econtext,
			   bool *isNull, ExprDoneCond *isDone)
{
	Datum		result = (Datum) 0;
	MinMaxExpr *minmax = (MinMaxExpr *) minmaxExpr->xprstate.expr;
	Oid			collation = minmax->inputcollid;
	MinMaxOp	op = minmax->op;
	FunctionCallInfoData locfcinfo;
	ListCell   *arg;

	if (isDone)
		*isDone = ExprSingleResult;
	*isNull = true;				/* until we get a result */

	InitFunctionCallInfoData(locfcinfo, &minmaxExpr->cfunc, 2,
							 collation, NULL, NULL);
	locfcinfo.argnull[0] = false;
	locfcinfo.argnull[1] = false;

	foreach(arg, minmaxExpr->args)
	{
		ExprState  *e = (ExprState *) lfirst(arg);
		Datum		value;
		bool		valueIsNull;
		int32		cmpresult;

		value = ExecEvalExpr(e, econtext, &valueIsNull, NULL);
		if (valueIsNull)
			continue;			/* ignore NULL inputs */

		if (*isNull)
		{
			/* first nonnull input, adopt value */
			result = value;
			*isNull = false;
		}
		else
		{
			/* apply comparison function */
			locfcinfo.arg[0] = result;
			locfcinfo.arg[1] = value;
			locfcinfo.isnull = false;
			cmpresult = DatumGetInt32(FunctionCallInvoke(&locfcinfo));
			if (locfcinfo.isnull)		/* probably should not happen */
				continue;
			if (cmpresult > 0 && op == IS_LEAST)
				result = value;
			else if (cmpresult < 0 && op == IS_GREATEST)
				result = value;
		}
	}

	return result;
}

/* ----------------------------------------------------------------
 *		ExecEvalXml
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalXml(XmlExprState *xmlExpr, ExprContext *econtext,
			bool *isNull, ExprDoneCond *isDone)
{
	XmlExpr    *xexpr = (XmlExpr *) xmlExpr->xprstate.expr;
	Datum		value;
	bool		isnull;
	ListCell   *arg;
	ListCell   *narg;

	if (isDone)
		*isDone = ExprSingleResult;
	*isNull = true;				/* until we get a result */

	switch (xexpr->op)
	{
		case IS_XMLCONCAT:
			{
				List	   *values = NIL;

				foreach(arg, xmlExpr->args)
				{
					ExprState  *e = (ExprState *) lfirst(arg);

					value = ExecEvalExpr(e, econtext, &isnull, NULL);
					if (!isnull)
						values = lappend(values, DatumGetPointer(value));
				}

				if (list_length(values) > 0)
				{
					*isNull = false;
					return PointerGetDatum(xmlconcat(values));
				}
				else
					return (Datum) 0;
			}
			break;

		case IS_XMLFOREST:
			{
				StringInfoData buf;

				initStringInfo(&buf);
				forboth(arg, xmlExpr->named_args, narg, xexpr->arg_names)
				{
					ExprState  *e = (ExprState *) lfirst(arg);
					char	   *argname = strVal(lfirst(narg));

					value = ExecEvalExpr(e, econtext, &isnull, NULL);
					if (!isnull)
					{
						appendStringInfo(&buf, "<%s>%s</%s>",
										 argname,
										 map_sql_value_to_xml_value(value, exprType((Node *) e->expr), true),
										 argname);
						*isNull = false;
					}
				}

				if (*isNull)
				{
					pfree(buf.data);
					return (Datum) 0;
				}
				else
				{
					text	   *result;

					result = cstring_to_text_with_len(buf.data, buf.len);
					pfree(buf.data);

					return PointerGetDatum(result);
				}
			}
			break;

		case IS_XMLELEMENT:
			*isNull = false;
			return PointerGetDatum(xmlelement(xmlExpr, econtext));
			break;

		case IS_XMLPARSE:
			{
				ExprState  *e;
				text	   *data;
				bool		preserve_whitespace;

				/* arguments are known to be text, bool */
				Assert(list_length(xmlExpr->args) == 2);

				e = (ExprState *) linitial(xmlExpr->args);
				value = ExecEvalExpr(e, econtext, &isnull, NULL);
				if (isnull)
					return (Datum) 0;
				data = DatumGetTextP(value);

				e = (ExprState *) lsecond(xmlExpr->args);
				value = ExecEvalExpr(e, econtext, &isnull, NULL);
				if (isnull)		/* probably can't happen */
					return (Datum) 0;
				preserve_whitespace = DatumGetBool(value);

				*isNull = false;

				return PointerGetDatum(xmlparse(data,
												xexpr->xmloption,
												preserve_whitespace));
			}
			break;

		case IS_XMLPI:
			{
				ExprState  *e;
				text	   *arg;

				/* optional argument is known to be text */
				Assert(list_length(xmlExpr->args) <= 1);

				if (xmlExpr->args)
				{
					e = (ExprState *) linitial(xmlExpr->args);
					value = ExecEvalExpr(e, econtext, &isnull, NULL);
					if (isnull)
						arg = NULL;
					else
						arg = DatumGetTextP(value);
				}
				else
				{
					arg = NULL;
					isnull = false;
				}

				return PointerGetDatum(xmlpi(xexpr->name, arg, isnull, isNull));
			}
			break;

		case IS_XMLROOT:
			{
				ExprState  *e;
				xmltype    *data;
				text	   *version;
				int			standalone;

				/* arguments are known to be xml, text, int */
				Assert(list_length(xmlExpr->args) == 3);

				e = (ExprState *) linitial(xmlExpr->args);
				value = ExecEvalExpr(e, econtext, &isnull, NULL);
				if (isnull)
					return (Datum) 0;
				data = DatumGetXmlP(value);

				e = (ExprState *) lsecond(xmlExpr->args);
				value = ExecEvalExpr(e, econtext, &isnull, NULL);
				if (isnull)
					version = NULL;
				else
					version = DatumGetTextP(value);

				e = (ExprState *) lthird(xmlExpr->args);
				value = ExecEvalExpr(e, econtext, &isnull, NULL);
				standalone = DatumGetInt32(value);

				*isNull = false;

				return PointerGetDatum(xmlroot(data,
											   version,
											   standalone));
			}
			break;

		case IS_XMLSERIALIZE:
			{
				ExprState  *e;

				/* argument type is known to be xml */
				Assert(list_length(xmlExpr->args) == 1);

				e = (ExprState *) linitial(xmlExpr->args);
				value = ExecEvalExpr(e, econtext, &isnull, NULL);
				if (isnull)
					return (Datum) 0;

				*isNull = false;

				return PointerGetDatum(xmltotext_with_xmloption(DatumGetXmlP(value), xexpr->xmloption));
			}
			break;

		case IS_DOCUMENT:
			{
				ExprState  *e;

				/* optional argument is known to be xml */
				Assert(list_length(xmlExpr->args) == 1);

				e = (ExprState *) linitial(xmlExpr->args);
				value = ExecEvalExpr(e, econtext, &isnull, NULL);
				if (isnull)
					return (Datum) 0;
				else
				{
					*isNull = false;
					return BoolGetDatum(xml_is_document(DatumGetXmlP(value)));
				}
			}
			break;
	}

	elog(ERROR, "unrecognized XML operation");
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
ExecEvalNullIf(FuncExprState *nullIfExpr,
			   ExprContext *econtext,
			   bool *isNull, ExprDoneCond *isDone)
{
	Datum		result;
	FunctionCallInfo fcinfo;
	ExprDoneCond argDone;

	if (isDone)
		*isDone = ExprSingleResult;

	/*
	 * Initialize function cache if first time through
	 */
	if (nullIfExpr->func.fn_oid == InvalidOid)
	{
		NullIfExpr *op = (NullIfExpr *) nullIfExpr->xprstate.expr;

		init_fcache(op->opfuncid, op->inputcollid, nullIfExpr,
					econtext->ecxt_per_query_memory, true);
		Assert(!nullIfExpr->func.fn_retset);
	}

	/*
	 * Evaluate arguments
	 */
	fcinfo = &nullIfExpr->fcinfo_data;
	argDone = ExecEvalFuncArgs(fcinfo, nullIfExpr->args, econtext);
	if (argDone != ExprSingleResult)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("NULLIF does not support set arguments")));
	Assert(fcinfo->nargs == 2);

	/* if either argument is NULL they can't be equal */
	if (!fcinfo->argnull[0] && !fcinfo->argnull[1])
	{
		fcinfo->isnull = false;
		result = FunctionCallInvoke(fcinfo);
		/* if the arguments are equal return null */
		if (!fcinfo->isnull && DatumGetBool(result))
		{
			*isNull = true;
			return (Datum) 0;
		}
	}

	/* else return first argument */
	*isNull = fcinfo->argnull[0];
	return fcinfo->arg[0];
}

/* ----------------------------------------------------------------
 *		ExecEvalNullTest
 *
 *		Evaluate a NullTest node.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalNullTest(NullTestState *nstate,
				 ExprContext *econtext,
				 bool *isNull,
				 ExprDoneCond *isDone)
{
	NullTest   *ntest = (NullTest *) nstate->xprstate.expr;
	Datum		result;

	result = ExecEvalExpr(nstate->arg, econtext, isNull, isDone);

	if (isDone && *isDone == ExprEndResult)
		return result;			/* nothing to check */

	if (ntest->argisrow && !(*isNull))
	{
		HeapTupleHeader tuple;
		Oid			tupType;
		int32		tupTypmod;
		TupleDesc	tupDesc;
		HeapTupleData tmptup;
		int			att;

		tuple = DatumGetHeapTupleHeader(result);

		tupType = HeapTupleHeaderGetTypeId(tuple);
		tupTypmod = HeapTupleHeaderGetTypMod(tuple);

		/* Lookup tupdesc if first time through or if type changes */
		tupDesc = get_cached_rowtype(tupType, tupTypmod,
									 &nstate->argdesc, econtext);

		/*
		 * heap_attisnull needs a HeapTuple not a bare HeapTupleHeader.
		 */
		tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
		tmptup.t_data = tuple;

		for (att = 1; att <= tupDesc->natts; att++)
		{
			/* ignore dropped columns */
			if (tupDesc->attrs[att - 1]->attisdropped)
				continue;
			if (heap_attisnull(&tmptup, att))
			{
				/* null field disproves IS NOT NULL */
				if (ntest->nulltesttype == IS_NOT_NULL)
					return BoolGetDatum(false);
			}
			else
			{
				/* non-null field disproves IS NULL */
				if (ntest->nulltesttype == IS_NULL)
					return BoolGetDatum(false);
			}
		}

		return BoolGetDatum(true);
	}
	else
	{
		/* Simple scalar-argument case, or a null rowtype datum */
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
				return (Datum) 0;		/* keep compiler quiet */
		}
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
	ListCell   *l;

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
									format_type_be(ctest->resulttype)),
							 errdatatype(ctest->resulttype)));
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
					 * itself within a check expression for another domain.
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
										con->name),
								 errdomainconstraint(ctest->resulttype,
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
ExecEvalCoerceToDomainValue(ExprState *exprstate,
							ExprContext *econtext,
							bool *isNull, ExprDoneCond *isDone)
{
	if (isDone)
		*isDone = ExprSingleResult;
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
ExecEvalFieldSelect(FieldSelectState *fstate,
					ExprContext *econtext,
					bool *isNull,
					ExprDoneCond *isDone)
{
	FieldSelect *fselect = (FieldSelect *) fstate->xprstate.expr;
	AttrNumber	fieldnum = fselect->fieldnum;
	Datum		result;
	Datum		tupDatum;
	HeapTupleHeader tuple;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupDesc;
	Form_pg_attribute attr;
	HeapTupleData tmptup;

	tupDatum = ExecEvalExpr(fstate->arg, econtext, isNull, isDone);

	/* this test covers the isDone exception too: */
	if (*isNull)
		return tupDatum;

	tuple = DatumGetHeapTupleHeader(tupDatum);

	tupType = HeapTupleHeaderGetTypeId(tuple);
	tupTypmod = HeapTupleHeaderGetTypMod(tuple);

	/* Lookup tupdesc if first time through or if type changes */
	tupDesc = get_cached_rowtype(tupType, tupTypmod,
								 &fstate->argdesc, econtext);

	/*
	 * Find field's attr record.  Note we don't support system columns here: a
	 * datum tuple doesn't have valid values for most of the interesting
	 * system columns anyway.
	 */
	if (fieldnum <= 0)			/* should never happen */
		elog(ERROR, "unsupported reference to system column %d in FieldSelect",
			 fieldnum);
	if (fieldnum > tupDesc->natts)		/* should never happen */
		elog(ERROR, "attribute number %d exceeds number of columns %d",
			 fieldnum, tupDesc->natts);
	attr = tupDesc->attrs[fieldnum - 1];

	/* Check for dropped column, and force a NULL result if so */
	if (attr->attisdropped)
	{
		*isNull = true;
		return (Datum) 0;
	}

	/* Check for type mismatch --- possible after ALTER COLUMN TYPE? */
	/* As in ExecEvalScalarVar, we should but can't check typmod */
	if (fselect->resulttype != attr->atttypid)
		ereport(ERROR,
				(errmsg("attribute %d has wrong type", fieldnum),
				 errdetail("Table has type %s, but query expects %s.",
						   format_type_be(attr->atttypid),
						   format_type_be(fselect->resulttype))));

	/* heap_getattr needs a HeapTuple not a bare HeapTupleHeader */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
	tmptup.t_data = tuple;

	result = heap_getattr(&tmptup,
						  fieldnum,
						  tupDesc,
						  isNull);
	return result;
}

/* ----------------------------------------------------------------
 *		ExecEvalFieldStore
 *
 *		Evaluate a FieldStore node.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalFieldStore(FieldStoreState *fstate,
				   ExprContext *econtext,
				   bool *isNull,
				   ExprDoneCond *isDone)
{
	FieldStore *fstore = (FieldStore *) fstate->xprstate.expr;
	HeapTuple	tuple;
	Datum		tupDatum;
	TupleDesc	tupDesc;
	Datum	   *values;
	bool	   *isnull;
	Datum		save_datum;
	bool		save_isNull;
	ListCell   *l1,
			   *l2;

	tupDatum = ExecEvalExpr(fstate->arg, econtext, isNull, isDone);

	if (isDone && *isDone == ExprEndResult)
		return tupDatum;

	/* Lookup tupdesc if first time through or after rescan */
	tupDesc = get_cached_rowtype(fstore->resulttype, -1,
								 &fstate->argdesc, econtext);

	/* Allocate workspace */
	values = (Datum *) palloc(tupDesc->natts * sizeof(Datum));
	isnull = (bool *) palloc(tupDesc->natts * sizeof(bool));

	if (!*isNull)
	{
		/*
		 * heap_deform_tuple needs a HeapTuple not a bare HeapTupleHeader. We
		 * set all the fields in the struct just in case.
		 */
		HeapTupleHeader tuphdr;
		HeapTupleData tmptup;

		tuphdr = DatumGetHeapTupleHeader(tupDatum);
		tmptup.t_len = HeapTupleHeaderGetDatumLength(tuphdr);
		ItemPointerSetInvalid(&(tmptup.t_self));
		tmptup.t_tableOid = InvalidOid;
		tmptup.t_data = tuphdr;

		heap_deform_tuple(&tmptup, tupDesc, values, isnull);
	}
	else
	{
		/* Convert null input tuple into an all-nulls row */
		memset(isnull, true, tupDesc->natts * sizeof(bool));
	}

	/* Result is never null */
	*isNull = false;

	save_datum = econtext->caseValue_datum;
	save_isNull = econtext->caseValue_isNull;

	forboth(l1, fstate->newvals, l2, fstore->fieldnums)
	{
		ExprState  *newval = (ExprState *) lfirst(l1);
		AttrNumber	fieldnum = lfirst_int(l2);

		Assert(fieldnum > 0 && fieldnum <= tupDesc->natts);

		/*
		 * Use the CaseTestExpr mechanism to pass down the old value of the
		 * field being replaced; this is needed in case the newval is itself a
		 * FieldStore or ArrayRef that has to obtain and modify the old value.
		 * It's safe to reuse the CASE mechanism because there cannot be a
		 * CASE between here and where the value would be needed, and a field
		 * assignment can't be within a CASE either.  (So saving and restoring
		 * the caseValue is just paranoia, but let's do it anyway.)
		 */
		econtext->caseValue_datum = values[fieldnum - 1];
		econtext->caseValue_isNull = isnull[fieldnum - 1];

		values[fieldnum - 1] = ExecEvalExpr(newval,
											econtext,
											&isnull[fieldnum - 1],
											NULL);
	}

	econtext->caseValue_datum = save_datum;
	econtext->caseValue_isNull = save_isNull;

	tuple = heap_form_tuple(tupDesc, values, isnull);

	pfree(values);
	pfree(isnull);

	return HeapTupleGetDatum(tuple);
}

/* ----------------------------------------------------------------
 *		ExecEvalRelabelType
 *
 *		Evaluate a RelabelType node.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalRelabelType(GenericExprState *exprstate,
					ExprContext *econtext,
					bool *isNull, ExprDoneCond *isDone)
{
	return ExecEvalExpr(exprstate->arg, econtext, isNull, isDone);
}

/* ----------------------------------------------------------------
 *		ExecEvalCoerceViaIO
 *
 *		Evaluate a CoerceViaIO node.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalCoerceViaIO(CoerceViaIOState *iostate,
					ExprContext *econtext,
					bool *isNull, ExprDoneCond *isDone)
{
	Datum		result;
	Datum		inputval;
	char	   *string;

	inputval = ExecEvalExpr(iostate->arg, econtext, isNull, isDone);

	if (isDone && *isDone == ExprEndResult)
		return inputval;		/* nothing to do */

	if (*isNull)
		string = NULL;			/* output functions are not called on nulls */
	else
		string = OutputFunctionCall(&iostate->outfunc, inputval);

	result = InputFunctionCall(&iostate->infunc,
							   string,
							   iostate->intypioparam,
							   -1);

	/* The input function cannot change the null/not-null status */
	return result;
}

/* ----------------------------------------------------------------
 *		ExecEvalArrayCoerceExpr
 *
 *		Evaluate an ArrayCoerceExpr node.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalArrayCoerceExpr(ArrayCoerceExprState *astate,
						ExprContext *econtext,
						bool *isNull, ExprDoneCond *isDone)
{
	ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) astate->xprstate.expr;
	Datum		result;
	ArrayType  *array;
	FunctionCallInfoData locfcinfo;

	result = ExecEvalExpr(astate->arg, econtext, isNull, isDone);

	if (isDone && *isDone == ExprEndResult)
		return result;			/* nothing to do */
	if (*isNull)
		return result;			/* nothing to do */

	/*
	 * If it's binary-compatible, modify the element type in the array header,
	 * but otherwise leave the array as we received it.
	 */
	if (!OidIsValid(acoerce->elemfuncid))
	{
		/* Detoast input array if necessary, and copy in any case */
		array = DatumGetArrayTypePCopy(result);
		ARR_ELEMTYPE(array) = astate->resultelemtype;
		PG_RETURN_ARRAYTYPE_P(array);
	}

	/* Detoast input array if necessary, but don't make a useless copy */
	array = DatumGetArrayTypeP(result);

	/* Initialize function cache if first time through */
	if (astate->elemfunc.fn_oid == InvalidOid)
	{
		AclResult	aclresult;

		/* Check permission to call function */
		aclresult = pg_proc_aclcheck(acoerce->elemfuncid, GetUserId(),
									 ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_PROC,
						   get_func_name(acoerce->elemfuncid));
		InvokeFunctionExecuteHook(acoerce->elemfuncid);

		/* Set up the primary fmgr lookup information */
		fmgr_info_cxt(acoerce->elemfuncid, &(astate->elemfunc),
					  econtext->ecxt_per_query_memory);
		fmgr_info_set_expr((Node *) acoerce, &(astate->elemfunc));
	}

	/*
	 * Use array_map to apply the function to each array element.
	 *
	 * We pass on the desttypmod and isExplicit flags whether or not the
	 * function wants them.
	 *
	 * Note: coercion functions are assumed to not use collation.
	 */
	InitFunctionCallInfoData(locfcinfo, &(astate->elemfunc), 3,
							 InvalidOid, NULL, NULL);
	locfcinfo.arg[0] = PointerGetDatum(array);
	locfcinfo.arg[1] = Int32GetDatum(acoerce->resulttypmod);
	locfcinfo.arg[2] = BoolGetDatum(acoerce->isExplicit);
	locfcinfo.argnull[0] = false;
	locfcinfo.argnull[1] = false;
	locfcinfo.argnull[2] = false;

	return array_map(&locfcinfo, ARR_ELEMTYPE(array), astate->resultelemtype,
					 astate->amstate);
}

/* ----------------------------------------------------------------
 *		ExecEvalCurrentOfExpr
 *
 * The planner should convert CURRENT OF into a TidScan qualification, or some
 * other special handling in a ForeignScan node.  So we have to be able to do
 * ExecInitExpr on a CurrentOfExpr, but we shouldn't ever actually execute it.
 * If we get here, we suppose we must be dealing with CURRENT OF on a foreign
 * table whose FDW doesn't handle it, and complain accordingly.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalCurrentOfExpr(ExprState *exprstate, ExprContext *econtext,
					  bool *isNull, ExprDoneCond *isDone)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		   errmsg("WHERE CURRENT OF is not supported for this table type")));
	return 0;					/* keep compiler quiet */
}


/*
 * ExecEvalExprSwitchContext
 *
 * Same as ExecEvalExpr, but get into the right allocation context explicitly.
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
 * Any Aggref, WindowFunc, or SubPlan nodes found in the tree are added to the
 * lists of such nodes held by the parent PlanState. Otherwise, we do very
 * little initialization here other than building the state-node tree.	Any
 * nontrivial work associated with initializing runtime info for a node should
 * happen during the first actual evaluation of that node.	(This policy lets
 * us avoid work if the node is never actually evaluated.)
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

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	switch (nodeTag(node))
	{
		case T_Var:
			/* varattno == InvalidAttrNumber means it's a whole-row Var */
			if (((Var *) node)->varattno == InvalidAttrNumber)
			{
				WholeRowVarExprState *wstate = makeNode(WholeRowVarExprState);

				wstate->parent = parent;
				wstate->wrv_junkFilter = NULL;
				state = (ExprState *) wstate;
				state->evalfunc = (ExprStateEvalFunc) ExecEvalWholeRowVar;
			}
			else
			{
				state = (ExprState *) makeNode(ExprState);
				state->evalfunc = ExecEvalScalarVar;
			}
			break;
		case T_Const:
			state = (ExprState *) makeNode(ExprState);
			state->evalfunc = ExecEvalConst;
			break;
		case T_Param:
			state = (ExprState *) makeNode(ExprState);
			switch (((Param *) node)->paramkind)
			{
				case PARAM_EXEC:
					state->evalfunc = ExecEvalParamExec;
					break;
				case PARAM_EXTERN:
					state->evalfunc = ExecEvalParamExtern;
					break;
				default:
					elog(ERROR, "unrecognized paramkind: %d",
						 (int) ((Param *) node)->paramkind);
					break;
			}
			break;
		case T_CoerceToDomainValue:
			state = (ExprState *) makeNode(ExprState);
			state->evalfunc = ExecEvalCoerceToDomainValue;
			break;
		case T_CaseTestExpr:
			state = (ExprState *) makeNode(ExprState);
			state->evalfunc = ExecEvalCaseTestExpr;
			break;
		case T_Aggref:
			{
				Aggref	   *aggref = (Aggref *) node;
				AggrefExprState *astate = makeNode(AggrefExprState);

				astate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalAggref;
				if (parent && IsA(parent, AggState))
				{
					AggState   *aggstate = (AggState *) parent;
					int			naggs;

					aggstate->aggs = lcons(astate, aggstate->aggs);
					naggs = ++aggstate->numaggs;

					astate->args = (List *) ExecInitExpr((Expr *) aggref->args,
														 parent);

					/*
					 * Complain if the aggregate's arguments contain any
					 * aggregates; nested agg functions are semantically
					 * nonsensical.  (This should have been caught earlier,
					 * but we defend against it here anyway.)
					 */
					if (naggs != aggstate->numaggs)
						ereport(ERROR,
								(errcode(ERRCODE_GROUPING_ERROR),
						errmsg("aggregate function calls cannot be nested")));
				}
				else
				{
					/* planner messed up */
					elog(ERROR, "Aggref found in non-Agg plan node");
				}
				state = (ExprState *) astate;
			}
			break;
		case T_WindowFunc:
			{
				WindowFunc *wfunc = (WindowFunc *) node;
				WindowFuncExprState *wfstate = makeNode(WindowFuncExprState);

				wfstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalWindowFunc;
				if (parent && IsA(parent, WindowAggState))
				{
					WindowAggState *winstate = (WindowAggState *) parent;
					int			nfuncs;

					winstate->funcs = lcons(wfstate, winstate->funcs);
					nfuncs = ++winstate->numfuncs;
					if (wfunc->winagg)
						winstate->numaggs++;

					wfstate->args = (List *) ExecInitExpr((Expr *) wfunc->args,
														  parent);

					/*
					 * Complain if the windowfunc's arguments contain any
					 * windowfuncs; nested window functions are semantically
					 * nonsensical.  (This should have been caught earlier,
					 * but we defend against it here anyway.)
					 */
					if (nfuncs != winstate->numfuncs)
						ereport(ERROR,
								(errcode(ERRCODE_WINDOWING_ERROR),
						  errmsg("window function calls cannot be nested")));
				}
				else
				{
					/* planner messed up */
					elog(ERROR, "WindowFunc found in non-WindowAgg plan node");
				}
				state = (ExprState *) wfstate;
			}
			break;
		case T_ArrayRef:
			{
				ArrayRef   *aref = (ArrayRef *) node;
				ArrayRefExprState *astate = makeNode(ArrayRefExprState);

				astate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalArrayRef;
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

				fstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalFunc;
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

				fstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalOper;
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

				fstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalDistinct;
				fstate->args = (List *)
					ExecInitExpr((Expr *) distinctexpr->args, parent);
				fstate->func.fn_oid = InvalidOid;		/* not initialized */
				state = (ExprState *) fstate;
			}
			break;
		case T_NullIfExpr:
			{
				NullIfExpr *nullifexpr = (NullIfExpr *) node;
				FuncExprState *fstate = makeNode(FuncExprState);

				fstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalNullIf;
				fstate->args = (List *)
					ExecInitExpr((Expr *) nullifexpr->args, parent);
				fstate->func.fn_oid = InvalidOid;		/* not initialized */
				state = (ExprState *) fstate;
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *opexpr = (ScalarArrayOpExpr *) node;
				ScalarArrayOpExprState *sstate = makeNode(ScalarArrayOpExprState);

				sstate->fxprstate.xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalScalarArrayOp;
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

				switch (boolexpr->boolop)
				{
					case AND_EXPR:
						bstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalAnd;
						break;
					case OR_EXPR:
						bstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalOr;
						break;
					case NOT_EXPR:
						bstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalNot;
						break;
					default:
						elog(ERROR, "unrecognized boolop: %d",
							 (int) boolexpr->boolop);
						break;
				}
				bstate->args = (List *)
					ExecInitExpr((Expr *) boolexpr->args, parent);
				state = (ExprState *) bstate;
			}
			break;
		case T_SubPlan:
			{
				SubPlan    *subplan = (SubPlan *) node;
				SubPlanState *sstate;

				if (!parent)
					elog(ERROR, "SubPlan found with no parent plan");

				sstate = ExecInitSubPlan(subplan, parent);

				/* Add SubPlanState nodes to parent->subPlan */
				parent->subPlan = lappend(parent->subPlan, sstate);

				state = (ExprState *) sstate;
			}
			break;
		case T_AlternativeSubPlan:
			{
				AlternativeSubPlan *asplan = (AlternativeSubPlan *) node;
				AlternativeSubPlanState *asstate;

				if (!parent)
					elog(ERROR, "AlternativeSubPlan found with no parent plan");

				asstate = ExecInitAlternativeSubPlan(asplan, parent);

				state = (ExprState *) asstate;
			}
			break;
		case T_FieldSelect:
			{
				FieldSelect *fselect = (FieldSelect *) node;
				FieldSelectState *fstate = makeNode(FieldSelectState);

				fstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalFieldSelect;
				fstate->arg = ExecInitExpr(fselect->arg, parent);
				fstate->argdesc = NULL;
				state = (ExprState *) fstate;
			}
			break;
		case T_FieldStore:
			{
				FieldStore *fstore = (FieldStore *) node;
				FieldStoreState *fstate = makeNode(FieldStoreState);

				fstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalFieldStore;
				fstate->arg = ExecInitExpr(fstore->arg, parent);
				fstate->newvals = (List *) ExecInitExpr((Expr *) fstore->newvals, parent);
				fstate->argdesc = NULL;
				state = (ExprState *) fstate;
			}
			break;
		case T_RelabelType:
			{
				RelabelType *relabel = (RelabelType *) node;
				GenericExprState *gstate = makeNode(GenericExprState);

				gstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalRelabelType;
				gstate->arg = ExecInitExpr(relabel->arg, parent);
				state = (ExprState *) gstate;
			}
			break;
		case T_CoerceViaIO:
			{
				CoerceViaIO *iocoerce = (CoerceViaIO *) node;
				CoerceViaIOState *iostate = makeNode(CoerceViaIOState);
				Oid			iofunc;
				bool		typisvarlena;

				iostate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalCoerceViaIO;
				iostate->arg = ExecInitExpr(iocoerce->arg, parent);
				/* lookup the result type's input function */
				getTypeInputInfo(iocoerce->resulttype, &iofunc,
								 &iostate->intypioparam);
				fmgr_info(iofunc, &iostate->infunc);
				/* lookup the input type's output function */
				getTypeOutputInfo(exprType((Node *) iocoerce->arg),
								  &iofunc, &typisvarlena);
				fmgr_info(iofunc, &iostate->outfunc);
				state = (ExprState *) iostate;
			}
			break;
		case T_ArrayCoerceExpr:
			{
				ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) node;
				ArrayCoerceExprState *astate = makeNode(ArrayCoerceExprState);

				astate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalArrayCoerceExpr;
				astate->arg = ExecInitExpr(acoerce->arg, parent);
				astate->resultelemtype = get_element_type(acoerce->resulttype);
				if (astate->resultelemtype == InvalidOid)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("target type is not an array")));
				/* Arrays over domains aren't supported yet */
				Assert(getBaseType(astate->resultelemtype) ==
					   astate->resultelemtype);
				astate->elemfunc.fn_oid = InvalidOid;	/* not initialized */
				astate->amstate = (ArrayMapState *) palloc0(sizeof(ArrayMapState));
				state = (ExprState *) astate;
			}
			break;
		case T_ConvertRowtypeExpr:
			{
				ConvertRowtypeExpr *convert = (ConvertRowtypeExpr *) node;
				ConvertRowtypeExprState *cstate = makeNode(ConvertRowtypeExprState);

				cstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalConvertRowtype;
				cstate->arg = ExecInitExpr(convert->arg, parent);
				state = (ExprState *) cstate;
			}
			break;
		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;
				CaseExprState *cstate = makeNode(CaseExprState);
				List	   *outlist = NIL;
				ListCell   *l;

				cstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalCase;
				cstate->arg = ExecInitExpr(caseexpr->arg, parent);
				foreach(l, caseexpr->args)
				{
					CaseWhen   *when = (CaseWhen *) lfirst(l);
					CaseWhenState *wstate = makeNode(CaseWhenState);

					Assert(IsA(when, CaseWhen));
					wstate->xprstate.evalfunc = NULL;	/* not used */
					wstate->xprstate.expr = (Expr *) when;
					wstate->expr = ExecInitExpr(when->expr, parent);
					wstate->result = ExecInitExpr(when->result, parent);
					outlist = lappend(outlist, wstate);
				}
				cstate->args = outlist;
				cstate->defresult = ExecInitExpr(caseexpr->defresult, parent);
				state = (ExprState *) cstate;
			}
			break;
		case T_ArrayExpr:
			{
				ArrayExpr  *arrayexpr = (ArrayExpr *) node;
				ArrayExprState *astate = makeNode(ArrayExprState);
				List	   *outlist = NIL;
				ListCell   *l;

				astate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalArray;
				foreach(l, arrayexpr->elements)
				{
					Expr	   *e = (Expr *) lfirst(l);
					ExprState  *estate;

					estate = ExecInitExpr(e, parent);
					outlist = lappend(outlist, estate);
				}
				astate->elements = outlist;
				/* do one-time catalog lookup for type info */
				get_typlenbyvalalign(arrayexpr->element_typeid,
									 &astate->elemlength,
									 &astate->elembyval,
									 &astate->elemalign);
				state = (ExprState *) astate;
			}
			break;
		case T_RowExpr:
			{
				RowExpr    *rowexpr = (RowExpr *) node;
				RowExprState *rstate = makeNode(RowExprState);
				Form_pg_attribute *attrs;
				List	   *outlist = NIL;
				ListCell   *l;
				int			i;

				rstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalRow;
				/* Build tupdesc to describe result tuples */
				if (rowexpr->row_typeid == RECORDOID)
				{
					/* generic record, use runtime type assignment */
					rstate->tupdesc = ExecTypeFromExprList(rowexpr->args,
														   rowexpr->colnames);
					BlessTupleDesc(rstate->tupdesc);
					/* we won't need to redo this at runtime */
				}
				else
				{
					/* it's been cast to a named type, use that */
					rstate->tupdesc = lookup_rowtype_tupdesc_copy(rowexpr->row_typeid, -1);
				}
				/* Set up evaluation, skipping any deleted columns */
				Assert(list_length(rowexpr->args) <= rstate->tupdesc->natts);
				attrs = rstate->tupdesc->attrs;
				i = 0;
				foreach(l, rowexpr->args)
				{
					Expr	   *e = (Expr *) lfirst(l);
					ExprState  *estate;

					if (!attrs[i]->attisdropped)
					{
						/*
						 * Guard against ALTER COLUMN TYPE on rowtype since
						 * the RowExpr was created.  XXX should we check
						 * typmod too?	Not sure we can be sure it'll be the
						 * same.
						 */
						if (exprType((Node *) e) != attrs[i]->atttypid)
							ereport(ERROR,
									(errcode(ERRCODE_DATATYPE_MISMATCH),
									 errmsg("ROW() column has type %s instead of type %s",
										format_type_be(exprType((Node *) e)),
									   format_type_be(attrs[i]->atttypid))));
					}
					else
					{
						/*
						 * Ignore original expression and insert a NULL. We
						 * don't really care what type of NULL it is, so
						 * always make an int4 NULL.
						 */
						e = (Expr *) makeNullConst(INT4OID, -1, InvalidOid);
					}
					estate = ExecInitExpr(e, parent);
					outlist = lappend(outlist, estate);
					i++;
				}
				rstate->args = outlist;
				state = (ExprState *) rstate;
			}
			break;
		case T_RowCompareExpr:
			{
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;
				RowCompareExprState *rstate = makeNode(RowCompareExprState);
				int			nopers = list_length(rcexpr->opnos);
				List	   *outlist;
				ListCell   *l;
				ListCell   *l2;
				ListCell   *l3;
				int			i;

				rstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalRowCompare;
				Assert(list_length(rcexpr->largs) == nopers);
				outlist = NIL;
				foreach(l, rcexpr->largs)
				{
					Expr	   *e = (Expr *) lfirst(l);
					ExprState  *estate;

					estate = ExecInitExpr(e, parent);
					outlist = lappend(outlist, estate);
				}
				rstate->largs = outlist;
				Assert(list_length(rcexpr->rargs) == nopers);
				outlist = NIL;
				foreach(l, rcexpr->rargs)
				{
					Expr	   *e = (Expr *) lfirst(l);
					ExprState  *estate;

					estate = ExecInitExpr(e, parent);
					outlist = lappend(outlist, estate);
				}
				rstate->rargs = outlist;
				Assert(list_length(rcexpr->opfamilies) == nopers);
				rstate->funcs = (FmgrInfo *) palloc(nopers * sizeof(FmgrInfo));
				rstate->collations = (Oid *) palloc(nopers * sizeof(Oid));
				i = 0;
				forthree(l, rcexpr->opnos, l2, rcexpr->opfamilies, l3, rcexpr->inputcollids)
				{
					Oid			opno = lfirst_oid(l);
					Oid			opfamily = lfirst_oid(l2);
					Oid			inputcollid = lfirst_oid(l3);
					int			strategy;
					Oid			lefttype;
					Oid			righttype;
					Oid			proc;

					get_op_opfamily_properties(opno, opfamily, false,
											   &strategy,
											   &lefttype,
											   &righttype);
					proc = get_opfamily_proc(opfamily,
											 lefttype,
											 righttype,
											 BTORDER_PROC);

					/*
					 * If we enforced permissions checks on index support
					 * functions, we'd need to make a check here.  But the
					 * index support machinery doesn't do that, and neither
					 * does this code.
					 */
					fmgr_info(proc, &(rstate->funcs[i]));
					rstate->collations[i] = inputcollid;
					i++;
				}
				state = (ExprState *) rstate;
			}
			break;
		case T_CoalesceExpr:
			{
				CoalesceExpr *coalesceexpr = (CoalesceExpr *) node;
				CoalesceExprState *cstate = makeNode(CoalesceExprState);
				List	   *outlist = NIL;
				ListCell   *l;

				cstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalCoalesce;
				foreach(l, coalesceexpr->args)
				{
					Expr	   *e = (Expr *) lfirst(l);
					ExprState  *estate;

					estate = ExecInitExpr(e, parent);
					outlist = lappend(outlist, estate);
				}
				cstate->args = outlist;
				state = (ExprState *) cstate;
			}
			break;
		case T_MinMaxExpr:
			{
				MinMaxExpr *minmaxexpr = (MinMaxExpr *) node;
				MinMaxExprState *mstate = makeNode(MinMaxExprState);
				List	   *outlist = NIL;
				ListCell   *l;
				TypeCacheEntry *typentry;

				mstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalMinMax;
				foreach(l, minmaxexpr->args)
				{
					Expr	   *e = (Expr *) lfirst(l);
					ExprState  *estate;

					estate = ExecInitExpr(e, parent);
					outlist = lappend(outlist, estate);
				}
				mstate->args = outlist;
				/* Look up the btree comparison function for the datatype */
				typentry = lookup_type_cache(minmaxexpr->minmaxtype,
											 TYPECACHE_CMP_PROC);
				if (!OidIsValid(typentry->cmp_proc))
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_FUNCTION),
							 errmsg("could not identify a comparison function for type %s",
									format_type_be(minmaxexpr->minmaxtype))));

				/*
				 * If we enforced permissions checks on index support
				 * functions, we'd need to make a check here.  But the index
				 * support machinery doesn't do that, and neither does this
				 * code.
				 */
				fmgr_info(typentry->cmp_proc, &(mstate->cfunc));
				state = (ExprState *) mstate;
			}
			break;
		case T_XmlExpr:
			{
				XmlExpr    *xexpr = (XmlExpr *) node;
				XmlExprState *xstate = makeNode(XmlExprState);
				List	   *outlist;
				ListCell   *arg;

				xstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalXml;
				outlist = NIL;
				foreach(arg, xexpr->named_args)
				{
					Expr	   *e = (Expr *) lfirst(arg);
					ExprState  *estate;

					estate = ExecInitExpr(e, parent);
					outlist = lappend(outlist, estate);
				}
				xstate->named_args = outlist;

				outlist = NIL;
				foreach(arg, xexpr->args)
				{
					Expr	   *e = (Expr *) lfirst(arg);
					ExprState  *estate;

					estate = ExecInitExpr(e, parent);
					outlist = lappend(outlist, estate);
				}
				xstate->args = outlist;

				state = (ExprState *) xstate;
			}
			break;
		case T_NullTest:
			{
				NullTest   *ntest = (NullTest *) node;
				NullTestState *nstate = makeNode(NullTestState);

				nstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalNullTest;
				nstate->arg = ExecInitExpr(ntest->arg, parent);
				nstate->argdesc = NULL;
				state = (ExprState *) nstate;
			}
			break;
		case T_BooleanTest:
			{
				BooleanTest *btest = (BooleanTest *) node;
				GenericExprState *gstate = makeNode(GenericExprState);

				gstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalBooleanTest;
				gstate->arg = ExecInitExpr(btest->arg, parent);
				state = (ExprState *) gstate;
			}
			break;
		case T_CoerceToDomain:
			{
				CoerceToDomain *ctest = (CoerceToDomain *) node;
				CoerceToDomainState *cstate = makeNode(CoerceToDomainState);

				cstate->xprstate.evalfunc = (ExprStateEvalFunc) ExecEvalCoerceToDomain;
				cstate->arg = ExecInitExpr(ctest->arg, parent);
				cstate->constraints = GetDomainConstraints(ctest->resulttype);
				state = (ExprState *) cstate;
			}
			break;
		case T_CurrentOfExpr:
			state = (ExprState *) makeNode(ExprState);
			state->evalfunc = ExecEvalCurrentOfExpr;
			break;
		case T_TargetEntry:
			{
				TargetEntry *tle = (TargetEntry *) node;
				GenericExprState *gstate = makeNode(GenericExprState);

				gstate->xprstate.evalfunc = NULL;		/* not used */
				gstate->arg = ExecInitExpr(tle->expr, parent);
				state = (ExprState *) gstate;
			}
			break;
		case T_List:
			{
				List	   *outlist = NIL;
				ListCell   *l;

				foreach(l, (List *) node)
				{
					outlist = lappend(outlist,
									  ExecInitExpr((Expr *) lfirst(l),
												   parent));
				}
				/* Don't fall through to the "common" code below */
				return (ExprState *) outlist;
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
 * ExecPrepareExpr --- initialize for expression execution outside a normal
 * Plan tree context.
 *
 * This differs from ExecInitExpr in that we don't assume the caller is
 * already running in the EState's per-query context.  Also, we run the
 * passed expression tree through expression_planner() to prepare it for
 * execution.  (In ordinary Plan trees the regular planning process will have
 * made the appropriate transformations on expressions, but for standalone
 * expressions this won't have happened.)
 */
ExprState *
ExecPrepareExpr(Expr *node, EState *estate)
{
	ExprState  *result;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	node = expression_planner(node);

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
	ListCell   *l;

	/*
	 * debugging stuff
	 */
	EV_printf("ExecQual: qual is ");
	EV_nodeDisplay(qual);
	EV_printf("\n");

	/*
	 * Run in short-lived per-tuple context while computing expressions.
	 */
	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/*
	 * Evaluate the qual conditions one at a time.	If we find a FALSE result,
	 * we can stop evaluating and return FALSE --- the AND result must be
	 * FALSE.  Also, if we find a NULL result when resultForNull is FALSE, we
	 * can stop and return FALSE --- the AND result must be FALSE or NULL in
	 * that case, and the caller doesn't care which.
	 *
	 * If we get to the end of the list, we can return TRUE.  This will happen
	 * when the AND result is indeed TRUE, or when the AND result is NULL (one
	 * or more NULL subresult, with all the rest TRUE) and the caller has
	 * specified resultForNull = TRUE.
	 */
	result = true;

	foreach(l, qual)
	{
		ExprState  *clause = (ExprState *) lfirst(l);
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
	return list_length(targetlist);
}

/*
 * Number of items in a tlist, not including any resjunk items
 */
int
ExecCleanTargetListLength(List *targetlist)
{
	int			len = 0;
	ListCell   *tl;

	foreach(tl, targetlist)
	{
		TargetEntry *curTle = (TargetEntry *) lfirst(tl);

		Assert(IsA(curTle, TargetEntry));
		if (!curTle->resjunk)
			len++;
	}
	return len;
}

/*
 * ExecTargetList
 *		Evaluates a targetlist with respect to the given
 *		expression context.  Returns TRUE if we were able to create
 *		a result, FALSE if we have exhausted a set-valued expression.
 *
 * Results are stored into the passed values and isnull arrays.
 * The caller must provide an itemIsDone array that persists across calls.
 *
 * As with ExecEvalExpr, the caller should pass isDone = NULL if not
 * prepared to deal with sets of result tuples.  Otherwise, a return
 * of *isDone = ExprMultipleResult signifies a set element, and a return
 * of *isDone = ExprEndResult signifies end of the set of tuple.
 * We assume that *isDone has been initialized to ExprSingleResult by caller.
 */
static bool
ExecTargetList(List *targetlist,
			   ExprContext *econtext,
			   Datum *values,
			   bool *isnull,
			   ExprDoneCond *itemIsDone,
			   ExprDoneCond *isDone)
{
	MemoryContext oldContext;
	ListCell   *tl;
	bool		haveDoneSets;

	/*
	 * Run in short-lived per-tuple context while computing expressions.
	 */
	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/*
	 * evaluate all the expressions in the target list
	 */
	haveDoneSets = false;		/* any exhausted set exprs in tlist? */

	foreach(tl, targetlist)
	{
		GenericExprState *gstate = (GenericExprState *) lfirst(tl);
		TargetEntry *tle = (TargetEntry *) gstate->xprstate.expr;
		AttrNumber	resind = tle->resno - 1;

		values[resind] = ExecEvalExpr(gstate->arg,
									  econtext,
									  &isnull[resind],
									  &itemIsDone[resind]);

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
			 * all sets are done, so report that tlist expansion is complete.
			 */
			*isDone = ExprEndResult;
			MemoryContextSwitchTo(oldContext);
			return false;
		}
		else
		{
			/*
			 * We have some done and some undone sets.	Restart the done ones
			 * so that we can deliver a tuple (if possible).
			 */
			foreach(tl, targetlist)
			{
				GenericExprState *gstate = (GenericExprState *) lfirst(tl);
				TargetEntry *tle = (TargetEntry *) gstate->xprstate.expr;
				AttrNumber	resind = tle->resno - 1;

				if (itemIsDone[resind] == ExprEndResult)
				{
					values[resind] = ExecEvalExpr(gstate->arg,
												  econtext,
												  &isnull[resind],
												  &itemIsDone[resind]);

					if (itemIsDone[resind] == ExprEndResult)
					{
						/*
						 * Oh dear, this item is returning an empty set. Guess
						 * we can't make a tuple after all.
						 */
						*isDone = ExprEndResult;
						break;
					}
				}
			}

			/*
			 * If we cannot make a tuple because some sets are empty, we still
			 * have to cycle the nonempty sets to completion, else resources
			 * will not be released from subplans etc.
			 *
			 * XXX is that still necessary?
			 */
			if (*isDone == ExprEndResult)
			{
				foreach(tl, targetlist)
				{
					GenericExprState *gstate = (GenericExprState *) lfirst(tl);
					TargetEntry *tle = (TargetEntry *) gstate->xprstate.expr;
					AttrNumber	resind = tle->resno - 1;

					while (itemIsDone[resind] == ExprMultipleResult)
					{
						values[resind] = ExecEvalExpr(gstate->arg,
													  econtext,
													  &isnull[resind],
													  &itemIsDone[resind]);
					}
				}

				MemoryContextSwitchTo(oldContext);
				return false;
			}
		}
	}

	/* Report success */
	MemoryContextSwitchTo(oldContext);

	return true;
}

/*
 * ExecProject
 *
 *		projects a tuple based on projection info and stores
 *		it in the previously specified tuple table slot.
 *
 *		Note: the result is always a virtual tuple; therefore it
 *		may reference the contents of the exprContext's scan tuples
 *		and/or temporary results constructed in the exprContext.
 *		If the caller wishes the result to be valid longer than that
 *		data will be valid, he must call ExecMaterializeSlot on the
 *		result slot.
 */
TupleTableSlot *
ExecProject(ProjectionInfo *projInfo, ExprDoneCond *isDone)
{
	TupleTableSlot *slot;
	ExprContext *econtext;
	int			numSimpleVars;

	/*
	 * sanity checks
	 */
	Assert(projInfo != NULL);

	/*
	 * get the projection info we want
	 */
	slot = projInfo->pi_slot;
	econtext = projInfo->pi_exprContext;

	/* Assume single result row until proven otherwise */
	if (isDone)
		*isDone = ExprSingleResult;

	/*
	 * Clear any former contents of the result slot.  This makes it safe for
	 * us to use the slot's Datum/isnull arrays as workspace. (Also, we can
	 * return the slot as-is if we decide no rows can be projected.)
	 */
	ExecClearTuple(slot);

	/*
	 * Force extraction of all input values that we'll need.  The
	 * Var-extraction loops below depend on this, and we are also prefetching
	 * all attributes that will be referenced in the generic expressions.
	 */
	if (projInfo->pi_lastInnerVar > 0)
		slot_getsomeattrs(econtext->ecxt_innertuple,
						  projInfo->pi_lastInnerVar);
	if (projInfo->pi_lastOuterVar > 0)
		slot_getsomeattrs(econtext->ecxt_outertuple,
						  projInfo->pi_lastOuterVar);
	if (projInfo->pi_lastScanVar > 0)
		slot_getsomeattrs(econtext->ecxt_scantuple,
						  projInfo->pi_lastScanVar);

	/*
	 * Assign simple Vars to result by direct extraction of fields from source
	 * slots ... a mite ugly, but fast ...
	 */
	numSimpleVars = projInfo->pi_numSimpleVars;
	if (numSimpleVars > 0)
	{
		Datum	   *values = slot->tts_values;
		bool	   *isnull = slot->tts_isnull;
		int		   *varSlotOffsets = projInfo->pi_varSlotOffsets;
		int		   *varNumbers = projInfo->pi_varNumbers;
		int			i;

		if (projInfo->pi_directMap)
		{
			/* especially simple case where vars go to output in order */
			for (i = 0; i < numSimpleVars; i++)
			{
				char	   *slotptr = ((char *) econtext) + varSlotOffsets[i];
				TupleTableSlot *varSlot = *((TupleTableSlot **) slotptr);
				int			varNumber = varNumbers[i] - 1;

				values[i] = varSlot->tts_values[varNumber];
				isnull[i] = varSlot->tts_isnull[varNumber];
			}
		}
		else
		{
			/* we have to pay attention to varOutputCols[] */
			int		   *varOutputCols = projInfo->pi_varOutputCols;

			for (i = 0; i < numSimpleVars; i++)
			{
				char	   *slotptr = ((char *) econtext) + varSlotOffsets[i];
				TupleTableSlot *varSlot = *((TupleTableSlot **) slotptr);
				int			varNumber = varNumbers[i] - 1;
				int			varOutputCol = varOutputCols[i] - 1;

				values[varOutputCol] = varSlot->tts_values[varNumber];
				isnull[varOutputCol] = varSlot->tts_isnull[varNumber];
			}
		}
	}

	/*
	 * If there are any generic expressions, evaluate them.  It's possible
	 * that there are set-returning functions in such expressions; if so and
	 * we have reached the end of the set, we return the result slot, which we
	 * already marked empty.
	 */
	if (projInfo->pi_targetlist)
	{
		if (!ExecTargetList(projInfo->pi_targetlist,
							econtext,
							slot->tts_values,
							slot->tts_isnull,
							projInfo->pi_itemIsDone,
							isDone))
			return slot;		/* no more result rows, return empty slot */
	}

	/*
	 * Successfully formed a result row.  Mark the result slot as containing a
	 * valid virtual tuple.
	 */
	return ExecStoreVirtualTuple(slot);
}
