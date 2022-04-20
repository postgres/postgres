/*-------------------------------------------------------------------------
 *
 * arraysubs.c
 *	  Subscripting support functions for arrays.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/arraysubs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/execExpr.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/subscripting.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


/* SubscriptingRefState.workspace for array subscripting execution */
typedef struct ArraySubWorkspace
{
	/* Values determined during expression compilation */
	Oid			refelemtype;	/* OID of the array element type */
	int16		refattrlength;	/* typlen of array type */
	int16		refelemlength;	/* typlen of the array element type */
	bool		refelembyval;	/* is the element type pass-by-value? */
	char		refelemalign;	/* typalign of the element type */

	/*
	 * Subscript values converted to integers.  Note that these arrays must be
	 * of length MAXDIM even when dealing with fewer subscripts, because
	 * array_get/set_slice may scribble on the extra entries.
	 */
	int			upperindex[MAXDIM];
	int			lowerindex[MAXDIM];
} ArraySubWorkspace;


/*
 * Finish parse analysis of a SubscriptingRef expression for an array.
 *
 * Transform the subscript expressions, coerce them to integers,
 * and determine the result type of the SubscriptingRef node.
 */
static void
array_subscript_transform(SubscriptingRef *sbsref,
						  List *indirection,
						  ParseState *pstate,
						  bool isSlice,
						  bool isAssignment)
{
	List	   *upperIndexpr = NIL;
	List	   *lowerIndexpr = NIL;
	ListCell   *idx;

	/*
	 * Transform the subscript expressions, and separate upper and lower
	 * bounds into two lists.
	 *
	 * If we have a container slice expression, we convert any non-slice
	 * indirection items to slices by treating the single subscript as the
	 * upper bound and supplying an assumed lower bound of 1.
	 */
	foreach(idx, indirection)
	{
		A_Indices  *ai = lfirst_node(A_Indices, idx);
		Node	   *subexpr;

		if (isSlice)
		{
			if (ai->lidx)
			{
				subexpr = transformExpr(pstate, ai->lidx, pstate->p_expr_kind);
				/* If it's not int4 already, try to coerce */
				subexpr = coerce_to_target_type(pstate,
												subexpr, exprType(subexpr),
												INT4OID, -1,
												COERCION_ASSIGNMENT,
												COERCE_IMPLICIT_CAST,
												-1);
				if (subexpr == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("array subscript must have type integer"),
							 parser_errposition(pstate, exprLocation(ai->lidx))));
			}
			else if (!ai->is_slice)
			{
				/* Make a constant 1 */
				subexpr = (Node *) makeConst(INT4OID,
											 -1,
											 InvalidOid,
											 sizeof(int32),
											 Int32GetDatum(1),
											 false,
											 true); /* pass by value */
			}
			else
			{
				/* Slice with omitted lower bound, put NULL into the list */
				subexpr = NULL;
			}
			lowerIndexpr = lappend(lowerIndexpr, subexpr);
		}
		else
			Assert(ai->lidx == NULL && !ai->is_slice);

		if (ai->uidx)
		{
			subexpr = transformExpr(pstate, ai->uidx, pstate->p_expr_kind);
			/* If it's not int4 already, try to coerce */
			subexpr = coerce_to_target_type(pstate,
											subexpr, exprType(subexpr),
											INT4OID, -1,
											COERCION_ASSIGNMENT,
											COERCE_IMPLICIT_CAST,
											-1);
			if (subexpr == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("array subscript must have type integer"),
						 parser_errposition(pstate, exprLocation(ai->uidx))));
		}
		else
		{
			/* Slice with omitted upper bound, put NULL into the list */
			Assert(isSlice && ai->is_slice);
			subexpr = NULL;
		}
		upperIndexpr = lappend(upperIndexpr, subexpr);
	}

	/* ... and store the transformed lists into the SubscriptRef node */
	sbsref->refupperindexpr = upperIndexpr;
	sbsref->reflowerindexpr = lowerIndexpr;

	/* Verify subscript list lengths are within implementation limit */
	if (list_length(upperIndexpr) > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
						list_length(upperIndexpr), MAXDIM)));
	/* We need not check lowerIndexpr separately */

	/*
	 * Determine the result type of the subscripting operation.  It's the same
	 * as the array type if we're slicing, else it's the element type.  In
	 * either case, the typmod is the same as the array's, so we need not
	 * change reftypmod.
	 */
	if (isSlice)
		sbsref->refrestype = sbsref->refcontainertype;
	else
		sbsref->refrestype = sbsref->refelemtype;
}

/*
 * During execution, process the subscripts in a SubscriptingRef expression.
 *
 * The subscript expressions are already evaluated in Datum form in the
 * SubscriptingRefState's arrays.  Check and convert them as necessary.
 *
 * If any subscript is NULL, we throw error in assignment cases, or in fetch
 * cases set result to NULL and return false (instructing caller to skip the
 * rest of the SubscriptingRef sequence).
 *
 * We convert all the subscripts to plain integers and save them in the
 * sbsrefstate->workspace arrays.
 */
static bool
array_subscript_check_subscripts(ExprState *state,
								 ExprEvalStep *op,
								 ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref_subscript.state;
	ArraySubWorkspace *workspace = (ArraySubWorkspace *) sbsrefstate->workspace;

	/* Process upper subscripts */
	for (int i = 0; i < sbsrefstate->numupper; i++)
	{
		if (sbsrefstate->upperprovided[i])
		{
			/* If any index expr yields NULL, result is NULL or error */
			if (sbsrefstate->upperindexnull[i])
			{
				if (sbsrefstate->isassignment)
					ereport(ERROR,
							(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							 errmsg("array subscript in assignment must not be null")));
				*op->resnull = true;
				return false;
			}
			workspace->upperindex[i] = DatumGetInt32(sbsrefstate->upperindex[i]);
		}
	}

	/* Likewise for lower subscripts */
	for (int i = 0; i < sbsrefstate->numlower; i++)
	{
		if (sbsrefstate->lowerprovided[i])
		{
			/* If any index expr yields NULL, result is NULL or error */
			if (sbsrefstate->lowerindexnull[i])
			{
				if (sbsrefstate->isassignment)
					ereport(ERROR,
							(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							 errmsg("array subscript in assignment must not be null")));
				*op->resnull = true;
				return false;
			}
			workspace->lowerindex[i] = DatumGetInt32(sbsrefstate->lowerindex[i]);
		}
	}

	return true;
}

/*
 * Evaluate SubscriptingRef fetch for an array element.
 *
 * Source container is in step's result variable (it's known not NULL, since
 * we set fetch_strict to true), and indexes have already been evaluated into
 * workspace array.
 */
static void
array_subscript_fetch(ExprState *state,
					  ExprEvalStep *op,
					  ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	ArraySubWorkspace *workspace = (ArraySubWorkspace *) sbsrefstate->workspace;

	/* Should not get here if source array (or any subscript) is null */
	Assert(!(*op->resnull));

	*op->resvalue = array_get_element(*op->resvalue,
									  sbsrefstate->numupper,
									  workspace->upperindex,
									  workspace->refattrlength,
									  workspace->refelemlength,
									  workspace->refelembyval,
									  workspace->refelemalign,
									  op->resnull);
}

/*
 * Evaluate SubscriptingRef fetch for an array slice.
 *
 * Source container is in step's result variable (it's known not NULL, since
 * we set fetch_strict to true), and indexes have already been evaluated into
 * workspace array.
 */
static void
array_subscript_fetch_slice(ExprState *state,
							ExprEvalStep *op,
							ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	ArraySubWorkspace *workspace = (ArraySubWorkspace *) sbsrefstate->workspace;

	/* Should not get here if source array (or any subscript) is null */
	Assert(!(*op->resnull));

	*op->resvalue = array_get_slice(*op->resvalue,
									sbsrefstate->numupper,
									workspace->upperindex,
									workspace->lowerindex,
									sbsrefstate->upperprovided,
									sbsrefstate->lowerprovided,
									workspace->refattrlength,
									workspace->refelemlength,
									workspace->refelembyval,
									workspace->refelemalign);
	/* The slice is never NULL, so no need to change *op->resnull */
}

/*
 * Evaluate SubscriptingRef assignment for an array element assignment.
 *
 * Input container (possibly null) is in result area, replacement value is in
 * SubscriptingRefState's replacevalue/replacenull.
 */
static void
array_subscript_assign(ExprState *state,
					   ExprEvalStep *op,
					   ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	ArraySubWorkspace *workspace = (ArraySubWorkspace *) sbsrefstate->workspace;
	Datum		arraySource = *op->resvalue;

	/*
	 * For an assignment to a fixed-length array type, both the original array
	 * and the value to be assigned into it must be non-NULL, else we punt and
	 * return the original array.
	 */
	if (workspace->refattrlength > 0)
	{
		if (*op->resnull || sbsrefstate->replacenull)
			return;
	}

	/*
	 * For assignment to varlena arrays, we handle a NULL original array by
	 * substituting an empty (zero-dimensional) array; insertion of the new
	 * element will result in a singleton array value.  It does not matter
	 * whether the new element is NULL.
	 */
	if (*op->resnull)
	{
		arraySource = PointerGetDatum(construct_empty_array(workspace->refelemtype));
		*op->resnull = false;
	}

	*op->resvalue = array_set_element(arraySource,
									  sbsrefstate->numupper,
									  workspace->upperindex,
									  sbsrefstate->replacevalue,
									  sbsrefstate->replacenull,
									  workspace->refattrlength,
									  workspace->refelemlength,
									  workspace->refelembyval,
									  workspace->refelemalign);
	/* The result is never NULL, so no need to change *op->resnull */
}

/*
 * Evaluate SubscriptingRef assignment for an array slice assignment.
 *
 * Input container (possibly null) is in result area, replacement value is in
 * SubscriptingRefState's replacevalue/replacenull.
 */
static void
array_subscript_assign_slice(ExprState *state,
							 ExprEvalStep *op,
							 ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	ArraySubWorkspace *workspace = (ArraySubWorkspace *) sbsrefstate->workspace;
	Datum		arraySource = *op->resvalue;

	/*
	 * For an assignment to a fixed-length array type, both the original array
	 * and the value to be assigned into it must be non-NULL, else we punt and
	 * return the original array.
	 */
	if (workspace->refattrlength > 0)
	{
		if (*op->resnull || sbsrefstate->replacenull)
			return;
	}

	/*
	 * For assignment to varlena arrays, we handle a NULL original array by
	 * substituting an empty (zero-dimensional) array; insertion of the new
	 * element will result in a singleton array value.  It does not matter
	 * whether the new element is NULL.
	 */
	if (*op->resnull)
	{
		arraySource = PointerGetDatum(construct_empty_array(workspace->refelemtype));
		*op->resnull = false;
	}

	*op->resvalue = array_set_slice(arraySource,
									sbsrefstate->numupper,
									workspace->upperindex,
									workspace->lowerindex,
									sbsrefstate->upperprovided,
									sbsrefstate->lowerprovided,
									sbsrefstate->replacevalue,
									sbsrefstate->replacenull,
									workspace->refattrlength,
									workspace->refelemlength,
									workspace->refelembyval,
									workspace->refelemalign);
	/* The result is never NULL, so no need to change *op->resnull */
}

/*
 * Compute old array element value for a SubscriptingRef assignment
 * expression.  Will only be called if the new-value subexpression
 * contains SubscriptingRef or FieldStore.  This is the same as the
 * regular fetch case, except that we have to handle a null array,
 * and the value should be stored into the SubscriptingRefState's
 * prevvalue/prevnull fields.
 */
static void
array_subscript_fetch_old(ExprState *state,
						  ExprEvalStep *op,
						  ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	ArraySubWorkspace *workspace = (ArraySubWorkspace *) sbsrefstate->workspace;

	if (*op->resnull)
	{
		/* whole array is null, so any element is too */
		sbsrefstate->prevvalue = (Datum) 0;
		sbsrefstate->prevnull = true;
	}
	else
		sbsrefstate->prevvalue = array_get_element(*op->resvalue,
												   sbsrefstate->numupper,
												   workspace->upperindex,
												   workspace->refattrlength,
												   workspace->refelemlength,
												   workspace->refelembyval,
												   workspace->refelemalign,
												   &sbsrefstate->prevnull);
}

/*
 * Compute old array slice value for a SubscriptingRef assignment
 * expression.  Will only be called if the new-value subexpression
 * contains SubscriptingRef or FieldStore.  This is the same as the
 * regular fetch case, except that we have to handle a null array,
 * and the value should be stored into the SubscriptingRefState's
 * prevvalue/prevnull fields.
 *
 * Note: this is presently dead code, because the new value for a
 * slice would have to be an array, so it couldn't directly contain a
 * FieldStore; nor could it contain a SubscriptingRef assignment, since
 * we consider adjacent subscripts to index one multidimensional array
 * not nested array types.  Future generalizations might make this
 * reachable, however.
 */
static void
array_subscript_fetch_old_slice(ExprState *state,
								ExprEvalStep *op,
								ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	ArraySubWorkspace *workspace = (ArraySubWorkspace *) sbsrefstate->workspace;

	if (*op->resnull)
	{
		/* whole array is null, so any slice is too */
		sbsrefstate->prevvalue = (Datum) 0;
		sbsrefstate->prevnull = true;
	}
	else
	{
		sbsrefstate->prevvalue = array_get_slice(*op->resvalue,
												 sbsrefstate->numupper,
												 workspace->upperindex,
												 workspace->lowerindex,
												 sbsrefstate->upperprovided,
												 sbsrefstate->lowerprovided,
												 workspace->refattrlength,
												 workspace->refelemlength,
												 workspace->refelembyval,
												 workspace->refelemalign);
		/* slices of non-null arrays are never null */
		sbsrefstate->prevnull = false;
	}
}

/*
 * Set up execution state for an array subscript operation.
 */
static void
array_exec_setup(const SubscriptingRef *sbsref,
				 SubscriptingRefState *sbsrefstate,
				 SubscriptExecSteps *methods)
{
	bool		is_slice = (sbsrefstate->numlower != 0);
	ArraySubWorkspace *workspace;

	/*
	 * Enforce the implementation limit on number of array subscripts.  This
	 * check isn't entirely redundant with checking at parse time; conceivably
	 * the expression was stored by a backend with a different MAXDIM value.
	 */
	if (sbsrefstate->numupper > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
						sbsrefstate->numupper, MAXDIM)));

	/* Should be impossible if parser is sane, but check anyway: */
	if (sbsrefstate->numlower != 0 &&
		sbsrefstate->numupper != sbsrefstate->numlower)
		elog(ERROR, "upper and lower index lists are not same length");

	/*
	 * Allocate type-specific workspace.
	 */
	workspace = (ArraySubWorkspace *) palloc(sizeof(ArraySubWorkspace));
	sbsrefstate->workspace = workspace;

	/*
	 * Collect datatype details we'll need at execution.
	 */
	workspace->refelemtype = sbsref->refelemtype;
	workspace->refattrlength = get_typlen(sbsref->refcontainertype);
	get_typlenbyvalalign(sbsref->refelemtype,
						 &workspace->refelemlength,
						 &workspace->refelembyval,
						 &workspace->refelemalign);

	/*
	 * Pass back pointers to appropriate step execution functions.
	 */
	methods->sbs_check_subscripts = array_subscript_check_subscripts;
	if (is_slice)
	{
		methods->sbs_fetch = array_subscript_fetch_slice;
		methods->sbs_assign = array_subscript_assign_slice;
		methods->sbs_fetch_old = array_subscript_fetch_old_slice;
	}
	else
	{
		methods->sbs_fetch = array_subscript_fetch;
		methods->sbs_assign = array_subscript_assign;
		methods->sbs_fetch_old = array_subscript_fetch_old;
	}
}

/*
 * array_subscript_handler
 *		Subscripting handler for standard varlena arrays.
 *
 * This should be used only for "true" array types, which have array headers
 * as understood by the varlena array routines, and are referenced by the
 * element type's pg_type.typarray field.
 */
Datum
array_subscript_handler(PG_FUNCTION_ARGS)
{
	static const SubscriptRoutines sbsroutines = {
		.transform = array_subscript_transform,
		.exec_setup = array_exec_setup,
		.fetch_strict = true,	/* fetch returns NULL for NULL inputs */
		.fetch_leakproof = true,	/* fetch returns NULL for bad subscript */
		.store_leakproof = false	/* ... but assignment throws error */
	};

	PG_RETURN_POINTER(&sbsroutines);
}

/*
 * raw_array_subscript_handler
 *		Subscripting handler for "raw" arrays.
 *
 * A "raw" array just contains N independent instances of the element type.
 * Currently we require both the element type and the array type to be fixed
 * length, but it wouldn't be too hard to relax that for the array type.
 *
 * As of now, all the support code is shared with standard varlena arrays.
 * We may split those into separate code paths, but probably that would yield
 * only marginal speedups.  The main point of having a separate handler is
 * so that pg_type.typsubscript clearly indicates the type's semantics.
 */
Datum
raw_array_subscript_handler(PG_FUNCTION_ARGS)
{
	static const SubscriptRoutines sbsroutines = {
		.transform = array_subscript_transform,
		.exec_setup = array_exec_setup,
		.fetch_strict = true,	/* fetch returns NULL for NULL inputs */
		.fetch_leakproof = true,	/* fetch returns NULL for bad subscript */
		.store_leakproof = false	/* ... but assignment throws error */
	};

	PG_RETURN_POINTER(&sbsroutines);
}
