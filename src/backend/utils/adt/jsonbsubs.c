/*-------------------------------------------------------------------------
 *
 * jsonbsubs.c
 *	  Subscripting support functions for jsonb.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonbsubs.c
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
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"


/*
 * SubscriptingRefState.workspace for generic jsonb subscripting execution.
 *
 * Stores state for both jsonb simple subscripting and dot notation access.
 * Dot notation additionally uses `jsonpath` for JsonPath evaluation.
 */
typedef struct JsonbSubWorkspace
{
	bool		expectArray;	/* jsonb root is expected to be an array */
	Oid		   *indexOid;		/* OID of coerced subscript expression, could
								 * be only integer or text */
	Datum	   *index;			/* Subscript values in Datum format */
	JsonPath   *jsonpath;		/* JsonPath for dot notation execution via
								 * JsonPathQuery() */
} JsonbSubWorkspace;

static Oid
jsonb_subscript_type(Node *expr)
{
	if (expr && IsA(expr, String))
		return TEXTOID;

	return exprType(expr);
}

static Node *
coerce_jsonpath_subscript(ParseState *pstate, Node *subExpr, Oid numtype)
{
	Oid			subExprType = jsonb_subscript_type(subExpr);
	Oid			targetType = UNKNOWNOID;

	if (subExprType != UNKNOWNOID)
	{
		Oid			targets[2] = {numtype, TEXTOID};

		/*
		 * Jsonb can handle multiple subscript types, but cases when a
		 * subscript could be coerced to multiple target types must be
		 * avoided, similar to overloaded functions. It could be possibly
		 * extend with jsonpath in the future.
		 */
		for (int i = 0; i < 2; i++)
		{
			if (can_coerce_type(1, &subExprType, &targets[i], COERCION_IMPLICIT))
			{
				/*
				 * One type has already succeeded, it means there are two
				 * coercion targets possible, failure.
				 */
				if (targetType != UNKNOWNOID)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("subscript type %s is not supported", format_type_be(subExprType)),
							 errhint("jsonb subscript must be coercible to only one type, integer or text."),
							 parser_errposition(pstate, exprLocation(subExpr))));

				targetType = targets[i];
			}
		}

		/*
		 * No suitable types were found, failure.
		 */
		if (targetType == UNKNOWNOID)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("subscript type %s is not supported", format_type_be(subExprType)),
					 errhint("jsonb subscript must be coercible to either integer or text."),
					 parser_errposition(pstate, exprLocation(subExpr))));
	}
	else
		targetType = TEXTOID;

	/*
	 * We known from can_coerce_type that coercion will succeed, so
	 * coerce_type could be used. Note the implicit coercion context, which is
	 * required to handle subscripts of different types, similar to overloaded
	 * functions.
	 */
	subExpr = coerce_type(pstate,
						  subExpr, subExprType,
						  targetType, -1,
						  COERCION_IMPLICIT,
						  COERCE_IMPLICIT_CAST,
						  -1);
	if (subExpr == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("jsonb subscript must have text type"),
				 parser_errposition(pstate, exprLocation(subExpr))));

	return subExpr;
}

/*
 * During transformation, determine whether to build a JsonPath
 * for JsonPathQuery() execution.
 *
 * JsonPath is needed if the indirection list includes:
 * - String-based access (dot notation)
 * - Wildcard (`*`)
 * - Slice-based subscripting
 *
 * Otherwise, simple jsonb subscripting is sufficient.
 */
static bool
jsonb_check_jsonpath_needed(List *indirection)
{
	ListCell   *lc;

	foreach(lc, indirection)
	{
		Node	   *accessor = lfirst(lc);

		if (IsA(accessor, String) ||
			IsA(accessor, A_Star))
			return true;
		else
		{
			A_Indices  *ai;

			Assert(IsA(accessor, A_Indices));
			ai = castNode(A_Indices, accessor);

			if (!ai->uidx || ai->lidx)
			{
				Assert(ai->is_slice);
				return true;
			}
		}
	}

	return false;
}

/*
 * Helper functions for constructing JsonPath expressions.
 *
 * The following functions create various types of JsonPathParseItem nodes,
 * which are used to build JsonPath expressions for jsonb simplified accessor.
 */

static JsonPathParseItem *
make_jsonpath_item(JsonPathItemType type)
{
	JsonPathParseItem *v = palloc(sizeof(*v));

	v->type = type;
	v->next = NULL;

	return v;
}

static JsonPathParseItem *
make_jsonpath_item_int(int32 val, List **exprs)
{
	JsonPathParseItem *jpi = make_jsonpath_item(jpiNumeric);

	jpi->value.numeric =
		DatumGetNumeric(DirectFunctionCall1(int4_numeric, Int32GetDatum(val)));

	*exprs = lappend(*exprs, makeConst(INT4OID, -1, InvalidOid, 4,
									   Int32GetDatum(val), false, true));

	return jpi;
}

/*
 * Convert an expression into a JsonPathParseItem.
 * If the expression is a constant integer, create a direct numeric item.
 * Otherwise, create a variable reference and add it to the expression list.
 */
static JsonPathParseItem *
make_jsonpath_item_expr(ParseState *pstate, Node *expr, List **exprs)
{
	Const	   *cnst;

	expr = transformExpr(pstate, expr, pstate->p_expr_kind);

	if (!IsA(expr, Const))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("jsonb simplified accessor supports subscripting in const int4, got type: %s",
						format_type_be(exprType(expr))),
				 parser_errposition(pstate, exprLocation(expr))));

	cnst = (Const *) expr;

	if (cnst->consttype == INT4OID && !cnst->constisnull)
	{
		int32		val = DatumGetInt32(cnst->constvalue);

		return make_jsonpath_item_int(val, exprs);
	}

	ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("jsonb simplified accessor supports subscripting in type: INT4, got type: %s",
					format_type_be(cnst->consttype)),
			 parser_errposition(pstate, exprLocation(expr))));
}

/*
 * jsonb_subscript_make_jsonpath
 *
 * Constructs a JsonPath expression from a list of indirections.
 * This function is used when jsonb subscripting involves dot notation,
 * wildcards (*), or slice-based subscripting, requiring JsonPath-based
 * evaluation.
 *
 * The function modifies the indirection list in place, removing processed
 * elements as it converts them into JsonPath components, as follows:
 * - String keys (dot notation) -> jpiKey items.
 * - Wildcard (*) -> jpiAnyKey item.
 * - Array indices and slices -> jpiIndexArray items.
 *
 * Parameters:
 * - pstate: Parse state context.
 * - indirection: List of subscripting expressions (modified in-place).
 * - uexprs: Upper-bound expressions extracted from subscripts.
 * - lexprs: Lower-bound expressions extracted from subscripts.
 * Returns:
 * - a Const node containing the transformed JsonPath expression.
 */
static Node *
jsonb_subscript_make_jsonpath(ParseState *pstate, List **indirection,
							  List **uexprs, List **lexprs)
{
	JsonPathParseResult jpres;
	JsonPathParseItem *path = make_jsonpath_item(jpiRoot);
	ListCell   *lc;
	Datum		jsp;
	int			pathlen = 0;

	*uexprs = NIL;
	*lexprs = NIL;

	jpres.expr = path;
	jpres.lax = true;

	foreach(lc, *indirection)
	{
		Node	   *accessor = lfirst(lc);
		JsonPathParseItem *jpi;

		if (IsA(accessor, String))
		{
			char	   *field = strVal(accessor);

			jpi = make_jsonpath_item(jpiKey);
			jpi->value.string.val = field;
			jpi->value.string.len = strlen(field);

			*uexprs = lappend(*uexprs, accessor);
		}
		else if (IsA(accessor, A_Star))
		{
			jpi = make_jsonpath_item(jpiAnyKey);

			*uexprs = lappend(*uexprs, NULL);
		}
		else if (IsA(accessor, A_Indices))
		{
			A_Indices  *ai = castNode(A_Indices, accessor);

			jpi = make_jsonpath_item(jpiIndexArray);
			jpi->value.array.nelems = 1;
			jpi->value.array.elems = palloc(sizeof(jpi->value.array.elems[0]));

			if (ai->is_slice)
			{
				while (list_length(*lexprs) < list_length(*uexprs))
					*lexprs = lappend(*lexprs, NULL);

				if (ai->lidx)
					jpi->value.array.elems[0].from = make_jsonpath_item_expr(pstate, ai->lidx, lexprs);
				else
					jpi->value.array.elems[0].from = make_jsonpath_item_int(0, lexprs);

				if (ai->uidx)
					jpi->value.array.elems[0].to = make_jsonpath_item_expr(pstate, ai->uidx, uexprs);
				else
				{
					jpi->value.array.elems[0].to = make_jsonpath_item(jpiLast);
					*uexprs = lappend(*uexprs, NULL);
				}
			}
			else
			{
				Assert(ai->uidx && !ai->lidx);
				jpi->value.array.elems[0].from = make_jsonpath_item_expr(pstate, ai->uidx, uexprs);
				jpi->value.array.elems[0].to = NULL;
			}
		}
		else
			break;

		/* append path item */
		path->next = jpi;
		path = jpi;
		pathlen++;
	}

	if (*lexprs)
	{
		while (list_length(*lexprs) < list_length(*uexprs))
			*lexprs = lappend(*lexprs, NULL);
	}

	*indirection = list_delete_first_n(*indirection, pathlen);

	jsp = jsonPathFromParseResult(&jpres, 0, NULL);

	return (Node *) makeConst(JSONPATHOID, -1, InvalidOid, -1, jsp, false, false);
}

/*
 * Finish parse analysis of a SubscriptingRef expression for a jsonb.
 *
 * Transform the subscript expressions, coerce them to text,
 * and determine the result type of the SubscriptingRef node.
 */
static void
jsonb_subscript_transform(SubscriptingRef *sbsref,
						  List **indirection,
						  ParseState *pstate,
						  bool isSlice,
						  bool isAssignment)
{
	List	   *upperIndexpr = NIL;
	ListCell   *idx;

	/* Determine the result type of the subscripting operation; always jsonb */
	sbsref->refrestype = JSONBOID;
	sbsref->reftypmod = -1;

	if (jsonb_check_jsonpath_needed(*indirection))
	{
		sbsref->refjsonbpath =
			jsonb_subscript_make_jsonpath(pstate, indirection,
										  &sbsref->refupperindexpr,
										  &sbsref->reflowerindexpr);
		return;
	}

	/*
	 * Transform and convert the subscript expressions. Jsonb subscripting
	 * does not support slices, look only and the upper index.
	 */
	foreach(idx, *indirection)
	{
		Node	   *i = lfirst(idx);
		A_Indices  *ai;
		Node	   *subExpr;

		Assert(IsA(i, A_Indices));

		ai = castNode(A_Indices, i);

		if (isSlice)
		{
			Node	   *expr = ai->uidx ? ai->uidx : ai->lidx;

			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("jsonb subscript does not support slices"),
					 parser_errposition(pstate, exprLocation(expr))));
		}

		if (ai->uidx)
		{
			subExpr = transformExpr(pstate, ai->uidx, pstate->p_expr_kind);
			subExpr = coerce_jsonpath_subscript(pstate, subExpr, INT4OID);
		}
		else
		{
			/*
			 * Slice with omitted upper bound. Should not happen as we already
			 * errored out on slice earlier, but handle this just in case.
			 */
			Assert(isSlice && ai->is_slice);
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("jsonb subscript does not support slices"),
					 parser_errposition(pstate, exprLocation(ai->uidx))));
		}

		upperIndexpr = lappend(upperIndexpr, subExpr);
	}

	/* store the transformed lists into the SubscriptRef node */
	sbsref->refupperindexpr = upperIndexpr;
	sbsref->reflowerindexpr = NIL;

	/* Remove processed elements */
	if (upperIndexpr)
		*indirection = list_delete_first_n(*indirection, list_length(upperIndexpr));
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
 */
static bool
jsonb_subscript_check_subscripts(ExprState *state,
								 ExprEvalStep *op,
								 ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref_subscript.state;
	JsonbSubWorkspace *workspace = (JsonbSubWorkspace *) sbsrefstate->workspace;

	/*
	 * In case if the first subscript is an integer, the source jsonb is
	 * expected to be an array. This information is not used directly, all
	 * such cases are handled within corresponding jsonb assign functions. But
	 * if the source jsonb is NULL the expected type will be used to construct
	 * an empty source.
	 */
	if (sbsrefstate->numupper > 0 && sbsrefstate->upperprovided[0] &&
		!sbsrefstate->upperindexnull[0] && workspace->indexOid[0] == INT4OID)
		workspace->expectArray = true;

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
							 errmsg("jsonb subscript in assignment must not be null")));
				*op->resnull = true;
				return false;
			}

			/*
			 * For jsonb fetch and assign functions we need to provide path in
			 * text format. Convert if it's not already text.
			 */
			if (!workspace->jsonpath && workspace->indexOid[i] == INT4OID)
			{
				Datum		datum = sbsrefstate->upperindex[i];
				char	   *cs = DatumGetCString(DirectFunctionCall1(int4out, datum));

				workspace->index[i] = CStringGetTextDatum(cs);
			}
			else
				workspace->index[i] = sbsrefstate->upperindex[i];
		}
	}

	return true;
}

/*
 * Evaluate SubscriptingRef fetch for a jsonb element.
 *
 * Source container is in step's result variable (it's known not NULL, since
 * we set fetch_strict to true).
 */
static void
jsonb_subscript_fetch(ExprState *state,
					  ExprEvalStep *op,
					  ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	JsonbSubWorkspace *workspace = (JsonbSubWorkspace *) sbsrefstate->workspace;

	/* Should not get here if source jsonb (or any subscript) is null */
	Assert(!(*op->resnull));

	if (workspace->jsonpath)
	{
		bool		empty = false;
		bool		error = false;

		*op->resvalue = JsonPathQuery(*op->resvalue, workspace->jsonpath,
									  JSW_CONDITIONAL,
									  &empty, &error, NULL,
									  NULL);

		*op->resnull = empty || error;
	}
	else
	{
		Jsonb	   *jsonbSource = DatumGetJsonbP(*op->resvalue);

		*op->resvalue = jsonb_get_element(jsonbSource,
										  workspace->index,
										  sbsrefstate->numupper,
										  op->resnull,
										  false);
	}
}

/*
 * Evaluate SubscriptingRef assignment for a jsonb element assignment.
 *
 * Input container (possibly null) is in result area, replacement value is in
 * SubscriptingRefState's replacevalue/replacenull.
 */
static void
jsonb_subscript_assign(ExprState *state,
					   ExprEvalStep *op,
					   ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	JsonbSubWorkspace *workspace = (JsonbSubWorkspace *) sbsrefstate->workspace;
	Jsonb	   *jsonbSource;
	JsonbValue	replacevalue;

	if (sbsrefstate->replacenull)
		replacevalue.type = jbvNull;
	else
		JsonbToJsonbValue(DatumGetJsonbP(sbsrefstate->replacevalue),
						  &replacevalue);

	/*
	 * In case if the input container is null, set up an empty jsonb and
	 * proceed with the assignment.
	 */
	if (*op->resnull)
	{
		JsonbValue	newSource;

		/*
		 * To avoid any surprising results, set up an empty jsonb array in
		 * case of an array is expected (i.e. the first subscript is integer),
		 * otherwise jsonb object.
		 */
		if (workspace->expectArray)
		{
			newSource.type = jbvArray;
			newSource.val.array.nElems = 0;
			newSource.val.array.rawScalar = false;
		}
		else
		{
			newSource.type = jbvObject;
			newSource.val.object.nPairs = 0;
		}

		jsonbSource = JsonbValueToJsonb(&newSource);
		*op->resnull = false;
	}
	else
		jsonbSource = DatumGetJsonbP(*op->resvalue);

	*op->resvalue = jsonb_set_element(jsonbSource,
									  workspace->index,
									  sbsrefstate->numupper,
									  &replacevalue);
	/* The result is never NULL, so no need to change *op->resnull */
}

/*
 * Compute old jsonb element value for a SubscriptingRef assignment
 * expression.  Will only be called if the new-value subexpression
 * contains SubscriptingRef or FieldStore.  This is the same as the
 * regular fetch case, except that we have to handle a null jsonb,
 * and the value should be stored into the SubscriptingRefState's
 * prevvalue/prevnull fields.
 */
static void
jsonb_subscript_fetch_old(ExprState *state,
						  ExprEvalStep *op,
						  ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;

	if (*op->resnull)
	{
		/* whole jsonb is null, so any element is too */
		sbsrefstate->prevvalue = (Datum) 0;
		sbsrefstate->prevnull = true;
	}
	else
	{
		Jsonb	   *jsonbSource = DatumGetJsonbP(*op->resvalue);

		sbsrefstate->prevvalue = jsonb_get_element(jsonbSource,
												   sbsrefstate->upperindex,
												   sbsrefstate->numupper,
												   &sbsrefstate->prevnull,
												   false);
	}
}

/*
 * Set up execution state for a jsonb subscript operation. Opposite to the
 * arrays subscription, there is no limit for number of subscripts as jsonb
 * type itself doesn't have nesting limits.
 */
static void
jsonb_exec_setup(const SubscriptingRef *sbsref,
				 SubscriptingRefState *sbsrefstate,
				 SubscriptExecSteps *methods)
{
	JsonbSubWorkspace *workspace;
	ListCell   *lc;
	int			nupper = sbsref->refupperindexpr->length;
	char	   *ptr;
	bool		useJsonpath = sbsref->refjsonbpath != NULL;

	/* Allocate type-specific workspace with space for per-subscript data */
	workspace = palloc0(MAXALIGN(sizeof(JsonbSubWorkspace)) +
						nupper * (sizeof(Datum) + sizeof(Oid)));
	workspace->expectArray = false;
	ptr = ((char *) workspace) + MAXALIGN(sizeof(JsonbSubWorkspace));

	if (useJsonpath)
		workspace->jsonpath = DatumGetJsonPathP(castNode(Const, sbsref->refjsonbpath)->constvalue);

	/*
	 * This coding assumes sizeof(Datum) >= sizeof(Oid), else we might
	 * misalign the indexOid pointer
	 */
	workspace->index = (Datum *) ptr;
	ptr += nupper * sizeof(Datum);
	workspace->indexOid = (Oid *) ptr;

	sbsrefstate->workspace = workspace;

	/* Collect subscript data types necessary at execution time */
	foreach(lc, sbsref->refupperindexpr)
	{
		Node	   *expr = lfirst(lc);
		int			i = foreach_current_index(lc);

		workspace->indexOid[i] = jsonb_subscript_type(expr);
	}

	/*
	 * Pass back pointers to appropriate step execution functions.
	 */
	methods->sbs_check_subscripts = jsonb_subscript_check_subscripts;
	methods->sbs_fetch = jsonb_subscript_fetch;
	methods->sbs_assign = jsonb_subscript_assign;
	methods->sbs_fetch_old = jsonb_subscript_fetch_old;
}

/*
 * jsonb_subscript_handler
 *		Subscripting handler for jsonb.
 *
 */
Datum
jsonb_subscript_handler(PG_FUNCTION_ARGS)
{
	static const SubscriptRoutines sbsroutines = {
		.transform = jsonb_subscript_transform,
		.exec_setup = jsonb_exec_setup,
		.fetch_strict = true,	/* fetch returns NULL for NULL inputs */
		.fetch_leakproof = true,	/* fetch returns NULL for bad subscript */
		.store_leakproof = false	/* ... but assignment throws error */
	};

	PG_RETURN_POINTER(&sbsroutines);
}
