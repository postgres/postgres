/*-------------------------------------------------------------------------
 *
 * nodeFuncs.c
 *		Various general-purpose manipulations of Node trees
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/nodeFuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

static bool expression_returns_set_walker(Node *node, void *context);
static int	leftmostLoc(int loc1, int loc2);
static bool fix_opfuncids_walker(Node *node, void *context);
static bool planstate_walk_subplans(List *plans,
									planstate_tree_walker_callback walker,
									void *context);
static bool planstate_walk_members(PlanState **planstates, int nplans,
								   planstate_tree_walker_callback walker,
								   void *context);


/*
 *	exprType -
 *	  returns the Oid of the type of the expression's result.
 */
Oid
exprType(const Node *expr)
{
	Oid			type;

	if (!expr)
		return InvalidOid;

	switch (nodeTag(expr))
	{
		case T_Var:
			type = ((const Var *) expr)->vartype;
			break;
		case T_Const:
			type = ((const Const *) expr)->consttype;
			break;
		case T_Param:
			type = ((const Param *) expr)->paramtype;
			break;
		case T_Aggref:
			type = ((const Aggref *) expr)->aggtype;
			break;
		case T_GroupingFunc:
			type = INT4OID;
			break;
		case T_WindowFunc:
			type = ((const WindowFunc *) expr)->wintype;
			break;
		case T_MergeSupportFunc:
			type = ((const MergeSupportFunc *) expr)->msftype;
			break;
		case T_SubscriptingRef:
			type = ((const SubscriptingRef *) expr)->refrestype;
			break;
		case T_FuncExpr:
			type = ((const FuncExpr *) expr)->funcresulttype;
			break;
		case T_NamedArgExpr:
			type = exprType((Node *) ((const NamedArgExpr *) expr)->arg);
			break;
		case T_OpExpr:
			type = ((const OpExpr *) expr)->opresulttype;
			break;
		case T_DistinctExpr:
			type = ((const DistinctExpr *) expr)->opresulttype;
			break;
		case T_NullIfExpr:
			type = ((const NullIfExpr *) expr)->opresulttype;
			break;
		case T_ScalarArrayOpExpr:
			type = BOOLOID;
			break;
		case T_BoolExpr:
			type = BOOLOID;
			break;
		case T_SubLink:
			{
				const SubLink *sublink = (const SubLink *) expr;

				if (sublink->subLinkType == EXPR_SUBLINK ||
					sublink->subLinkType == ARRAY_SUBLINK)
				{
					/* get the type of the subselect's first target column */
					Query	   *qtree = (Query *) sublink->subselect;
					TargetEntry *tent;

					if (!qtree || !IsA(qtree, Query))
						elog(ERROR, "cannot get type for untransformed sublink");
					tent = linitial_node(TargetEntry, qtree->targetList);
					Assert(!tent->resjunk);
					type = exprType((Node *) tent->expr);
					if (sublink->subLinkType == ARRAY_SUBLINK)
					{
						type = get_promoted_array_type(type);
						if (!OidIsValid(type))
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_OBJECT),
									 errmsg("could not find array type for data type %s",
											format_type_be(exprType((Node *) tent->expr)))));
					}
				}
				else if (sublink->subLinkType == MULTIEXPR_SUBLINK)
				{
					/* MULTIEXPR is always considered to return RECORD */
					type = RECORDOID;
				}
				else
				{
					/* for all other sublink types, result is boolean */
					type = BOOLOID;
				}
			}
			break;
		case T_SubPlan:
			{
				const SubPlan *subplan = (const SubPlan *) expr;

				if (subplan->subLinkType == EXPR_SUBLINK ||
					subplan->subLinkType == ARRAY_SUBLINK)
				{
					/* get the type of the subselect's first target column */
					type = subplan->firstColType;
					if (subplan->subLinkType == ARRAY_SUBLINK)
					{
						type = get_promoted_array_type(type);
						if (!OidIsValid(type))
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_OBJECT),
									 errmsg("could not find array type for data type %s",
											format_type_be(subplan->firstColType))));
					}
				}
				else if (subplan->subLinkType == MULTIEXPR_SUBLINK)
				{
					/* MULTIEXPR is always considered to return RECORD */
					type = RECORDOID;
				}
				else
				{
					/* for all other subplan types, result is boolean */
					type = BOOLOID;
				}
			}
			break;
		case T_AlternativeSubPlan:
			{
				const AlternativeSubPlan *asplan = (const AlternativeSubPlan *) expr;

				/* subplans should all return the same thing */
				type = exprType((Node *) linitial(asplan->subplans));
			}
			break;
		case T_FieldSelect:
			type = ((const FieldSelect *) expr)->resulttype;
			break;
		case T_FieldStore:
			type = ((const FieldStore *) expr)->resulttype;
			break;
		case T_RelabelType:
			type = ((const RelabelType *) expr)->resulttype;
			break;
		case T_CoerceViaIO:
			type = ((const CoerceViaIO *) expr)->resulttype;
			break;
		case T_ArrayCoerceExpr:
			type = ((const ArrayCoerceExpr *) expr)->resulttype;
			break;
		case T_ConvertRowtypeExpr:
			type = ((const ConvertRowtypeExpr *) expr)->resulttype;
			break;
		case T_CollateExpr:
			type = exprType((Node *) ((const CollateExpr *) expr)->arg);
			break;
		case T_CaseExpr:
			type = ((const CaseExpr *) expr)->casetype;
			break;
		case T_CaseTestExpr:
			type = ((const CaseTestExpr *) expr)->typeId;
			break;
		case T_ArrayExpr:
			type = ((const ArrayExpr *) expr)->array_typeid;
			break;
		case T_RowExpr:
			type = ((const RowExpr *) expr)->row_typeid;
			break;
		case T_RowCompareExpr:
			type = BOOLOID;
			break;
		case T_CoalesceExpr:
			type = ((const CoalesceExpr *) expr)->coalescetype;
			break;
		case T_MinMaxExpr:
			type = ((const MinMaxExpr *) expr)->minmaxtype;
			break;
		case T_SQLValueFunction:
			type = ((const SQLValueFunction *) expr)->type;
			break;
		case T_XmlExpr:
			if (((const XmlExpr *) expr)->op == IS_DOCUMENT)
				type = BOOLOID;
			else if (((const XmlExpr *) expr)->op == IS_XMLSERIALIZE)
				type = TEXTOID;
			else
				type = XMLOID;
			break;
		case T_JsonValueExpr:
			{
				const JsonValueExpr *jve = (const JsonValueExpr *) expr;

				type = exprType((Node *) jve->formatted_expr);
			}
			break;
		case T_JsonConstructorExpr:
			type = ((const JsonConstructorExpr *) expr)->returning->typid;
			break;
		case T_JsonIsPredicate:
			type = BOOLOID;
			break;
		case T_JsonExpr:
			{
				const JsonExpr *jexpr = (const JsonExpr *) expr;

				type = jexpr->returning->typid;
				break;
			}
		case T_JsonBehavior:
			{
				const JsonBehavior *behavior = (const JsonBehavior *) expr;

				type = exprType(behavior->expr);
				break;
			}
		case T_NullTest:
			type = BOOLOID;
			break;
		case T_BooleanTest:
			type = BOOLOID;
			break;
		case T_CoerceToDomain:
			type = ((const CoerceToDomain *) expr)->resulttype;
			break;
		case T_CoerceToDomainValue:
			type = ((const CoerceToDomainValue *) expr)->typeId;
			break;
		case T_SetToDefault:
			type = ((const SetToDefault *) expr)->typeId;
			break;
		case T_CurrentOfExpr:
			type = BOOLOID;
			break;
		case T_NextValueExpr:
			type = ((const NextValueExpr *) expr)->typeId;
			break;
		case T_InferenceElem:
			{
				const InferenceElem *n = (const InferenceElem *) expr;

				type = exprType((Node *) n->expr);
			}
			break;
		case T_PlaceHolderVar:
			type = exprType((Node *) ((const PlaceHolderVar *) expr)->phexpr);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(expr));
			type = InvalidOid;	/* keep compiler quiet */
			break;
	}
	return type;
}

/*
 *	exprTypmod -
 *	  returns the type-specific modifier of the expression's result type,
 *	  if it can be determined.  In many cases, it can't and we return -1.
 */
int32
exprTypmod(const Node *expr)
{
	if (!expr)
		return -1;

	switch (nodeTag(expr))
	{
		case T_Var:
			return ((const Var *) expr)->vartypmod;
		case T_Const:
			return ((const Const *) expr)->consttypmod;
		case T_Param:
			return ((const Param *) expr)->paramtypmod;
		case T_SubscriptingRef:
			return ((const SubscriptingRef *) expr)->reftypmod;
		case T_FuncExpr:
			{
				int32		coercedTypmod;

				/* Be smart about length-coercion functions... */
				if (exprIsLengthCoercion(expr, &coercedTypmod))
					return coercedTypmod;
			}
			break;
		case T_NamedArgExpr:
			return exprTypmod((Node *) ((const NamedArgExpr *) expr)->arg);
		case T_NullIfExpr:
			{
				/*
				 * Result is either first argument or NULL, so we can report
				 * first argument's typmod if known.
				 */
				const NullIfExpr *nexpr = (const NullIfExpr *) expr;

				return exprTypmod((Node *) linitial(nexpr->args));
			}
			break;
		case T_SubLink:
			{
				const SubLink *sublink = (const SubLink *) expr;

				if (sublink->subLinkType == EXPR_SUBLINK ||
					sublink->subLinkType == ARRAY_SUBLINK)
				{
					/* get the typmod of the subselect's first target column */
					Query	   *qtree = (Query *) sublink->subselect;
					TargetEntry *tent;

					if (!qtree || !IsA(qtree, Query))
						elog(ERROR, "cannot get type for untransformed sublink");
					tent = linitial_node(TargetEntry, qtree->targetList);
					Assert(!tent->resjunk);
					return exprTypmod((Node *) tent->expr);
					/* note we don't need to care if it's an array */
				}
				/* otherwise, result is RECORD or BOOLEAN, typmod is -1 */
			}
			break;
		case T_SubPlan:
			{
				const SubPlan *subplan = (const SubPlan *) expr;

				if (subplan->subLinkType == EXPR_SUBLINK ||
					subplan->subLinkType == ARRAY_SUBLINK)
				{
					/* get the typmod of the subselect's first target column */
					/* note we don't need to care if it's an array */
					return subplan->firstColTypmod;
				}
				/* otherwise, result is RECORD or BOOLEAN, typmod is -1 */
			}
			break;
		case T_AlternativeSubPlan:
			{
				const AlternativeSubPlan *asplan = (const AlternativeSubPlan *) expr;

				/* subplans should all return the same thing */
				return exprTypmod((Node *) linitial(asplan->subplans));
			}
			break;
		case T_FieldSelect:
			return ((const FieldSelect *) expr)->resulttypmod;
		case T_RelabelType:
			return ((const RelabelType *) expr)->resulttypmod;
		case T_ArrayCoerceExpr:
			return ((const ArrayCoerceExpr *) expr)->resulttypmod;
		case T_CollateExpr:
			return exprTypmod((Node *) ((const CollateExpr *) expr)->arg);
		case T_CaseExpr:
			{
				/*
				 * If all the alternatives agree on type/typmod, return that
				 * typmod, else use -1
				 */
				const CaseExpr *cexpr = (const CaseExpr *) expr;
				Oid			casetype = cexpr->casetype;
				int32		typmod;
				ListCell   *arg;

				if (!cexpr->defresult)
					return -1;
				if (exprType((Node *) cexpr->defresult) != casetype)
					return -1;
				typmod = exprTypmod((Node *) cexpr->defresult);
				if (typmod < 0)
					return -1;	/* no point in trying harder */
				foreach(arg, cexpr->args)
				{
					CaseWhen   *w = lfirst_node(CaseWhen, arg);

					if (exprType((Node *) w->result) != casetype)
						return -1;
					if (exprTypmod((Node *) w->result) != typmod)
						return -1;
				}
				return typmod;
			}
			break;
		case T_CaseTestExpr:
			return ((const CaseTestExpr *) expr)->typeMod;
		case T_ArrayExpr:
			{
				/*
				 * If all the elements agree on type/typmod, return that
				 * typmod, else use -1
				 */
				const ArrayExpr *arrayexpr = (const ArrayExpr *) expr;
				Oid			commontype;
				int32		typmod;
				ListCell   *elem;

				if (arrayexpr->elements == NIL)
					return -1;
				typmod = exprTypmod((Node *) linitial(arrayexpr->elements));
				if (typmod < 0)
					return -1;	/* no point in trying harder */
				if (arrayexpr->multidims)
					commontype = arrayexpr->array_typeid;
				else
					commontype = arrayexpr->element_typeid;
				foreach(elem, arrayexpr->elements)
				{
					Node	   *e = (Node *) lfirst(elem);

					if (exprType(e) != commontype)
						return -1;
					if (exprTypmod(e) != typmod)
						return -1;
				}
				return typmod;
			}
			break;
		case T_CoalesceExpr:
			{
				/*
				 * If all the alternatives agree on type/typmod, return that
				 * typmod, else use -1
				 */
				const CoalesceExpr *cexpr = (const CoalesceExpr *) expr;
				Oid			coalescetype = cexpr->coalescetype;
				int32		typmod;
				ListCell   *arg;

				if (exprType((Node *) linitial(cexpr->args)) != coalescetype)
					return -1;
				typmod = exprTypmod((Node *) linitial(cexpr->args));
				if (typmod < 0)
					return -1;	/* no point in trying harder */
				for_each_from(arg, cexpr->args, 1)
				{
					Node	   *e = (Node *) lfirst(arg);

					if (exprType(e) != coalescetype)
						return -1;
					if (exprTypmod(e) != typmod)
						return -1;
				}
				return typmod;
			}
			break;
		case T_MinMaxExpr:
			{
				/*
				 * If all the alternatives agree on type/typmod, return that
				 * typmod, else use -1
				 */
				const MinMaxExpr *mexpr = (const MinMaxExpr *) expr;
				Oid			minmaxtype = mexpr->minmaxtype;
				int32		typmod;
				ListCell   *arg;

				if (exprType((Node *) linitial(mexpr->args)) != minmaxtype)
					return -1;
				typmod = exprTypmod((Node *) linitial(mexpr->args));
				if (typmod < 0)
					return -1;	/* no point in trying harder */
				for_each_from(arg, mexpr->args, 1)
				{
					Node	   *e = (Node *) lfirst(arg);

					if (exprType(e) != minmaxtype)
						return -1;
					if (exprTypmod(e) != typmod)
						return -1;
				}
				return typmod;
			}
			break;
		case T_SQLValueFunction:
			return ((const SQLValueFunction *) expr)->typmod;
		case T_JsonValueExpr:
			return exprTypmod((Node *) ((const JsonValueExpr *) expr)->formatted_expr);
		case T_JsonConstructorExpr:
			return ((const JsonConstructorExpr *) expr)->returning->typmod;
		case T_JsonExpr:
			{
				const JsonExpr *jexpr = (const JsonExpr *) expr;

				return jexpr->returning->typmod;
			}
			break;
		case T_JsonBehavior:
			{
				const JsonBehavior *behavior = (const JsonBehavior *) expr;

				return exprTypmod(behavior->expr);
			}
			break;
		case T_CoerceToDomain:
			return ((const CoerceToDomain *) expr)->resulttypmod;
		case T_CoerceToDomainValue:
			return ((const CoerceToDomainValue *) expr)->typeMod;
		case T_SetToDefault:
			return ((const SetToDefault *) expr)->typeMod;
		case T_PlaceHolderVar:
			return exprTypmod((Node *) ((const PlaceHolderVar *) expr)->phexpr);
		default:
			break;
	}
	return -1;
}

/*
 * exprIsLengthCoercion
 *		Detect whether an expression tree is an application of a datatype's
 *		typmod-coercion function.  Optionally extract the result's typmod.
 *
 * If coercedTypmod is not NULL, the typmod is stored there if the expression
 * is a length-coercion function, else -1 is stored there.
 *
 * Note that a combined type-and-length coercion will be treated as a
 * length coercion by this routine.
 */
bool
exprIsLengthCoercion(const Node *expr, int32 *coercedTypmod)
{
	if (coercedTypmod != NULL)
		*coercedTypmod = -1;	/* default result on failure */

	/*
	 * Scalar-type length coercions are FuncExprs, array-type length coercions
	 * are ArrayCoerceExprs
	 */
	if (expr && IsA(expr, FuncExpr))
	{
		const FuncExpr *func = (const FuncExpr *) expr;
		int			nargs;
		Const	   *second_arg;

		/*
		 * If it didn't come from a coercion context, reject.
		 */
		if (func->funcformat != COERCE_EXPLICIT_CAST &&
			func->funcformat != COERCE_IMPLICIT_CAST)
			return false;

		/*
		 * If it's not a two-argument or three-argument function with the
		 * second argument being an int4 constant, it can't have been created
		 * from a length coercion (it must be a type coercion, instead).
		 */
		nargs = list_length(func->args);
		if (nargs < 2 || nargs > 3)
			return false;

		second_arg = (Const *) lsecond(func->args);
		if (!IsA(second_arg, Const) ||
			second_arg->consttype != INT4OID ||
			second_arg->constisnull)
			return false;

		/*
		 * OK, it is indeed a length-coercion function.
		 */
		if (coercedTypmod != NULL)
			*coercedTypmod = DatumGetInt32(second_arg->constvalue);

		return true;
	}

	if (expr && IsA(expr, ArrayCoerceExpr))
	{
		const ArrayCoerceExpr *acoerce = (const ArrayCoerceExpr *) expr;

		/* It's not a length coercion unless there's a nondefault typmod */
		if (acoerce->resulttypmod < 0)
			return false;

		/*
		 * OK, it is indeed a length-coercion expression.
		 */
		if (coercedTypmod != NULL)
			*coercedTypmod = acoerce->resulttypmod;

		return true;
	}

	return false;
}

/*
 * applyRelabelType
 *		Add a RelabelType node if needed to make the expression expose
 *		the specified type, typmod, and collation.
 *
 * This is primarily intended to be used during planning.  Therefore, it must
 * maintain the post-eval_const_expressions invariants that there are not
 * adjacent RelabelTypes, and that the tree is fully const-folded (hence,
 * we mustn't return a RelabelType atop a Const).  If we do find a Const,
 * we'll modify it in-place if "overwrite_ok" is true; that should only be
 * passed as true if caller knows the Const is newly generated.
 */
Node *
applyRelabelType(Node *arg, Oid rtype, int32 rtypmod, Oid rcollid,
				 CoercionForm rformat, int rlocation, bool overwrite_ok)
{
	/*
	 * If we find stacked RelabelTypes (eg, from foo::int::oid) we can discard
	 * all but the top one, and must do so to ensure that semantically
	 * equivalent expressions are equal().
	 */
	while (arg && IsA(arg, RelabelType))
		arg = (Node *) ((RelabelType *) arg)->arg;

	if (arg && IsA(arg, Const))
	{
		/* Modify the Const directly to preserve const-flatness. */
		Const	   *con = (Const *) arg;

		if (!overwrite_ok)
			con = copyObject(con);
		con->consttype = rtype;
		con->consttypmod = rtypmod;
		con->constcollid = rcollid;
		/* We keep the Const's original location. */
		return (Node *) con;
	}
	else if (exprType(arg) == rtype &&
			 exprTypmod(arg) == rtypmod &&
			 exprCollation(arg) == rcollid)
	{
		/* Sometimes we find a nest of relabels that net out to nothing. */
		return arg;
	}
	else
	{
		/* Nope, gotta have a RelabelType. */
		RelabelType *newrelabel = makeNode(RelabelType);

		newrelabel->arg = (Expr *) arg;
		newrelabel->resulttype = rtype;
		newrelabel->resulttypmod = rtypmod;
		newrelabel->resultcollid = rcollid;
		newrelabel->relabelformat = rformat;
		newrelabel->location = rlocation;
		return (Node *) newrelabel;
	}
}

/*
 * relabel_to_typmod
 *		Add a RelabelType node that changes just the typmod of the expression.
 *
 * Convenience function for a common usage of applyRelabelType.
 */
Node *
relabel_to_typmod(Node *expr, int32 typmod)
{
	return applyRelabelType(expr, exprType(expr), typmod, exprCollation(expr),
							COERCE_EXPLICIT_CAST, -1, false);
}

/*
 * strip_implicit_coercions: remove implicit coercions at top level of tree
 *
 * This doesn't modify or copy the input expression tree, just return a
 * pointer to a suitable place within it.
 *
 * Note: there isn't any useful thing we can do with a RowExpr here, so
 * just return it unchanged, even if it's marked as an implicit coercion.
 */
Node *
strip_implicit_coercions(Node *node)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, FuncExpr))
	{
		FuncExpr   *f = (FuncExpr *) node;

		if (f->funcformat == COERCE_IMPLICIT_CAST)
			return strip_implicit_coercions(linitial(f->args));
	}
	else if (IsA(node, RelabelType))
	{
		RelabelType *r = (RelabelType *) node;

		if (r->relabelformat == COERCE_IMPLICIT_CAST)
			return strip_implicit_coercions((Node *) r->arg);
	}
	else if (IsA(node, CoerceViaIO))
	{
		CoerceViaIO *c = (CoerceViaIO *) node;

		if (c->coerceformat == COERCE_IMPLICIT_CAST)
			return strip_implicit_coercions((Node *) c->arg);
	}
	else if (IsA(node, ArrayCoerceExpr))
	{
		ArrayCoerceExpr *c = (ArrayCoerceExpr *) node;

		if (c->coerceformat == COERCE_IMPLICIT_CAST)
			return strip_implicit_coercions((Node *) c->arg);
	}
	else if (IsA(node, ConvertRowtypeExpr))
	{
		ConvertRowtypeExpr *c = (ConvertRowtypeExpr *) node;

		if (c->convertformat == COERCE_IMPLICIT_CAST)
			return strip_implicit_coercions((Node *) c->arg);
	}
	else if (IsA(node, CoerceToDomain))
	{
		CoerceToDomain *c = (CoerceToDomain *) node;

		if (c->coercionformat == COERCE_IMPLICIT_CAST)
			return strip_implicit_coercions((Node *) c->arg);
	}
	return node;
}

/*
 * expression_returns_set
 *	  Test whether an expression returns a set result.
 *
 * Because we use expression_tree_walker(), this can also be applied to
 * whole targetlists; it'll produce true if any one of the tlist items
 * returns a set.
 */
bool
expression_returns_set(Node *clause)
{
	return expression_returns_set_walker(clause, NULL);
}

static bool
expression_returns_set_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, FuncExpr))
	{
		FuncExpr   *expr = (FuncExpr *) node;

		if (expr->funcretset)
			return true;
		/* else fall through to check args */
	}
	if (IsA(node, OpExpr))
	{
		OpExpr	   *expr = (OpExpr *) node;

		if (expr->opretset)
			return true;
		/* else fall through to check args */
	}

	/*
	 * If you add any more cases that return sets, also fix
	 * expression_returns_set_rows() in clauses.c and IS_SRF_CALL() in
	 * tlist.c.
	 */

	/* Avoid recursion for some cases that parser checks not to return a set */
	if (IsA(node, Aggref))
		return false;
	if (IsA(node, GroupingFunc))
		return false;
	if (IsA(node, WindowFunc))
		return false;

	return expression_tree_walker(node, expression_returns_set_walker,
								  context);
}


/*
 *	exprCollation -
 *	  returns the Oid of the collation of the expression's result.
 *
 * Note: expression nodes that can invoke functions generally have an
 * "inputcollid" field, which is what the function should use as collation.
 * That is the resolved common collation of the node's inputs.  It is often
 * but not always the same as the result collation; in particular, if the
 * function produces a non-collatable result type from collatable inputs
 * or vice versa, the two are different.
 */
Oid
exprCollation(const Node *expr)
{
	Oid			coll;

	if (!expr)
		return InvalidOid;

	switch (nodeTag(expr))
	{
		case T_Var:
			coll = ((const Var *) expr)->varcollid;
			break;
		case T_Const:
			coll = ((const Const *) expr)->constcollid;
			break;
		case T_Param:
			coll = ((const Param *) expr)->paramcollid;
			break;
		case T_Aggref:
			coll = ((const Aggref *) expr)->aggcollid;
			break;
		case T_GroupingFunc:
			coll = InvalidOid;
			break;
		case T_WindowFunc:
			coll = ((const WindowFunc *) expr)->wincollid;
			break;
		case T_MergeSupportFunc:
			coll = ((const MergeSupportFunc *) expr)->msfcollid;
			break;
		case T_SubscriptingRef:
			coll = ((const SubscriptingRef *) expr)->refcollid;
			break;
		case T_FuncExpr:
			coll = ((const FuncExpr *) expr)->funccollid;
			break;
		case T_NamedArgExpr:
			coll = exprCollation((Node *) ((const NamedArgExpr *) expr)->arg);
			break;
		case T_OpExpr:
			coll = ((const OpExpr *) expr)->opcollid;
			break;
		case T_DistinctExpr:
			coll = ((const DistinctExpr *) expr)->opcollid;
			break;
		case T_NullIfExpr:
			coll = ((const NullIfExpr *) expr)->opcollid;
			break;
		case T_ScalarArrayOpExpr:
			/* ScalarArrayOpExpr's result is boolean ... */
			coll = InvalidOid;	/* ... so it has no collation */
			break;
		case T_BoolExpr:
			/* BoolExpr's result is boolean ... */
			coll = InvalidOid;	/* ... so it has no collation */
			break;
		case T_SubLink:
			{
				const SubLink *sublink = (const SubLink *) expr;

				if (sublink->subLinkType == EXPR_SUBLINK ||
					sublink->subLinkType == ARRAY_SUBLINK)
				{
					/* get the collation of subselect's first target column */
					Query	   *qtree = (Query *) sublink->subselect;
					TargetEntry *tent;

					if (!qtree || !IsA(qtree, Query))
						elog(ERROR, "cannot get collation for untransformed sublink");
					tent = linitial_node(TargetEntry, qtree->targetList);
					Assert(!tent->resjunk);
					coll = exprCollation((Node *) tent->expr);
					/* collation doesn't change if it's converted to array */
				}
				else
				{
					/* otherwise, SubLink's result is RECORD or BOOLEAN */
					coll = InvalidOid;	/* ... so it has no collation */
				}
			}
			break;
		case T_SubPlan:
			{
				const SubPlan *subplan = (const SubPlan *) expr;

				if (subplan->subLinkType == EXPR_SUBLINK ||
					subplan->subLinkType == ARRAY_SUBLINK)
				{
					/* get the collation of subselect's first target column */
					coll = subplan->firstColCollation;
					/* collation doesn't change if it's converted to array */
				}
				else
				{
					/* otherwise, SubPlan's result is RECORD or BOOLEAN */
					coll = InvalidOid;	/* ... so it has no collation */
				}
			}
			break;
		case T_AlternativeSubPlan:
			{
				const AlternativeSubPlan *asplan = (const AlternativeSubPlan *) expr;

				/* subplans should all return the same thing */
				coll = exprCollation((Node *) linitial(asplan->subplans));
			}
			break;
		case T_FieldSelect:
			coll = ((const FieldSelect *) expr)->resultcollid;
			break;
		case T_FieldStore:
			/* FieldStore's result is composite ... */
			coll = InvalidOid;	/* ... so it has no collation */
			break;
		case T_RelabelType:
			coll = ((const RelabelType *) expr)->resultcollid;
			break;
		case T_CoerceViaIO:
			coll = ((const CoerceViaIO *) expr)->resultcollid;
			break;
		case T_ArrayCoerceExpr:
			coll = ((const ArrayCoerceExpr *) expr)->resultcollid;
			break;
		case T_ConvertRowtypeExpr:
			/* ConvertRowtypeExpr's result is composite ... */
			coll = InvalidOid;	/* ... so it has no collation */
			break;
		case T_CollateExpr:
			coll = ((const CollateExpr *) expr)->collOid;
			break;
		case T_CaseExpr:
			coll = ((const CaseExpr *) expr)->casecollid;
			break;
		case T_CaseTestExpr:
			coll = ((const CaseTestExpr *) expr)->collation;
			break;
		case T_ArrayExpr:
			coll = ((const ArrayExpr *) expr)->array_collid;
			break;
		case T_RowExpr:
			/* RowExpr's result is composite ... */
			coll = InvalidOid;	/* ... so it has no collation */
			break;
		case T_RowCompareExpr:
			/* RowCompareExpr's result is boolean ... */
			coll = InvalidOid;	/* ... so it has no collation */
			break;
		case T_CoalesceExpr:
			coll = ((const CoalesceExpr *) expr)->coalescecollid;
			break;
		case T_MinMaxExpr:
			coll = ((const MinMaxExpr *) expr)->minmaxcollid;
			break;
		case T_SQLValueFunction:
			/* Returns either NAME or a non-collatable type */
			if (((const SQLValueFunction *) expr)->type == NAMEOID)
				coll = C_COLLATION_OID;
			else
				coll = InvalidOid;
			break;
		case T_XmlExpr:

			/*
			 * XMLSERIALIZE returns text from non-collatable inputs, so its
			 * collation is always default.  The other cases return boolean or
			 * XML, which are non-collatable.
			 */
			if (((const XmlExpr *) expr)->op == IS_XMLSERIALIZE)
				coll = DEFAULT_COLLATION_OID;
			else
				coll = InvalidOid;
			break;
		case T_JsonValueExpr:
			coll = exprCollation((Node *) ((const JsonValueExpr *) expr)->formatted_expr);
			break;
		case T_JsonConstructorExpr:
			{
				const JsonConstructorExpr *ctor = (const JsonConstructorExpr *) expr;

				if (ctor->coercion)
					coll = exprCollation((Node *) ctor->coercion);
				else
					coll = InvalidOid;
			}
			break;
		case T_JsonIsPredicate:
			/* IS JSON's result is boolean ... */
			coll = InvalidOid;	/* ... so it has no collation */
			break;
		case T_JsonExpr:
			{
				const JsonExpr *jsexpr = (JsonExpr *) expr;

				coll = jsexpr->collation;
			}
			break;
		case T_JsonBehavior:
			{
				const JsonBehavior *behavior = (JsonBehavior *) expr;

				if (behavior->expr)
					coll = exprCollation(behavior->expr);
				else
					coll = InvalidOid;
			}
			break;
		case T_NullTest:
			/* NullTest's result is boolean ... */
			coll = InvalidOid;	/* ... so it has no collation */
			break;
		case T_BooleanTest:
			/* BooleanTest's result is boolean ... */
			coll = InvalidOid;	/* ... so it has no collation */
			break;
		case T_CoerceToDomain:
			coll = ((const CoerceToDomain *) expr)->resultcollid;
			break;
		case T_CoerceToDomainValue:
			coll = ((const CoerceToDomainValue *) expr)->collation;
			break;
		case T_SetToDefault:
			coll = ((const SetToDefault *) expr)->collation;
			break;
		case T_CurrentOfExpr:
			/* CurrentOfExpr's result is boolean ... */
			coll = InvalidOid;	/* ... so it has no collation */
			break;
		case T_NextValueExpr:
			/* NextValueExpr's result is an integer type ... */
			coll = InvalidOid;	/* ... so it has no collation */
			break;
		case T_InferenceElem:
			coll = exprCollation((Node *) ((const InferenceElem *) expr)->expr);
			break;
		case T_PlaceHolderVar:
			coll = exprCollation((Node *) ((const PlaceHolderVar *) expr)->phexpr);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(expr));
			coll = InvalidOid;	/* keep compiler quiet */
			break;
	}
	return coll;
}

/*
 *	exprInputCollation -
 *	  returns the Oid of the collation a function should use, if available.
 *
 * Result is InvalidOid if the node type doesn't store this information.
 */
Oid
exprInputCollation(const Node *expr)
{
	Oid			coll;

	if (!expr)
		return InvalidOid;

	switch (nodeTag(expr))
	{
		case T_Aggref:
			coll = ((const Aggref *) expr)->inputcollid;
			break;
		case T_WindowFunc:
			coll = ((const WindowFunc *) expr)->inputcollid;
			break;
		case T_FuncExpr:
			coll = ((const FuncExpr *) expr)->inputcollid;
			break;
		case T_OpExpr:
			coll = ((const OpExpr *) expr)->inputcollid;
			break;
		case T_DistinctExpr:
			coll = ((const DistinctExpr *) expr)->inputcollid;
			break;
		case T_NullIfExpr:
			coll = ((const NullIfExpr *) expr)->inputcollid;
			break;
		case T_ScalarArrayOpExpr:
			coll = ((const ScalarArrayOpExpr *) expr)->inputcollid;
			break;
		case T_MinMaxExpr:
			coll = ((const MinMaxExpr *) expr)->inputcollid;
			break;
		default:
			coll = InvalidOid;
			break;
	}
	return coll;
}

/*
 *	exprSetCollation -
 *	  Assign collation information to an expression tree node.
 *
 * Note: since this is only used during parse analysis, we don't need to
 * worry about subplans or PlaceHolderVars.
 */
void
exprSetCollation(Node *expr, Oid collation)
{
	switch (nodeTag(expr))
	{
		case T_Var:
			((Var *) expr)->varcollid = collation;
			break;
		case T_Const:
			((Const *) expr)->constcollid = collation;
			break;
		case T_Param:
			((Param *) expr)->paramcollid = collation;
			break;
		case T_Aggref:
			((Aggref *) expr)->aggcollid = collation;
			break;
		case T_GroupingFunc:
			Assert(!OidIsValid(collation));
			break;
		case T_WindowFunc:
			((WindowFunc *) expr)->wincollid = collation;
			break;
		case T_MergeSupportFunc:
			((MergeSupportFunc *) expr)->msfcollid = collation;
			break;
		case T_SubscriptingRef:
			((SubscriptingRef *) expr)->refcollid = collation;
			break;
		case T_FuncExpr:
			((FuncExpr *) expr)->funccollid = collation;
			break;
		case T_NamedArgExpr:
			Assert(collation == exprCollation((Node *) ((NamedArgExpr *) expr)->arg));
			break;
		case T_OpExpr:
			((OpExpr *) expr)->opcollid = collation;
			break;
		case T_DistinctExpr:
			((DistinctExpr *) expr)->opcollid = collation;
			break;
		case T_NullIfExpr:
			((NullIfExpr *) expr)->opcollid = collation;
			break;
		case T_ScalarArrayOpExpr:
			/* ScalarArrayOpExpr's result is boolean ... */
			Assert(!OidIsValid(collation)); /* ... so never set a collation */
			break;
		case T_BoolExpr:
			/* BoolExpr's result is boolean ... */
			Assert(!OidIsValid(collation)); /* ... so never set a collation */
			break;
		case T_SubLink:
#ifdef USE_ASSERT_CHECKING
			{
				SubLink    *sublink = (SubLink *) expr;

				if (sublink->subLinkType == EXPR_SUBLINK ||
					sublink->subLinkType == ARRAY_SUBLINK)
				{
					/* get the collation of subselect's first target column */
					Query	   *qtree = (Query *) sublink->subselect;
					TargetEntry *tent;

					if (!qtree || !IsA(qtree, Query))
						elog(ERROR, "cannot set collation for untransformed sublink");
					tent = linitial_node(TargetEntry, qtree->targetList);
					Assert(!tent->resjunk);
					Assert(collation == exprCollation((Node *) tent->expr));
				}
				else
				{
					/* otherwise, result is RECORD or BOOLEAN */
					Assert(!OidIsValid(collation));
				}
			}
#endif							/* USE_ASSERT_CHECKING */
			break;
		case T_FieldSelect:
			((FieldSelect *) expr)->resultcollid = collation;
			break;
		case T_FieldStore:
			/* FieldStore's result is composite ... */
			Assert(!OidIsValid(collation)); /* ... so never set a collation */
			break;
		case T_RelabelType:
			((RelabelType *) expr)->resultcollid = collation;
			break;
		case T_CoerceViaIO:
			((CoerceViaIO *) expr)->resultcollid = collation;
			break;
		case T_ArrayCoerceExpr:
			((ArrayCoerceExpr *) expr)->resultcollid = collation;
			break;
		case T_ConvertRowtypeExpr:
			/* ConvertRowtypeExpr's result is composite ... */
			Assert(!OidIsValid(collation)); /* ... so never set a collation */
			break;
		case T_CaseExpr:
			((CaseExpr *) expr)->casecollid = collation;
			break;
		case T_ArrayExpr:
			((ArrayExpr *) expr)->array_collid = collation;
			break;
		case T_RowExpr:
			/* RowExpr's result is composite ... */
			Assert(!OidIsValid(collation)); /* ... so never set a collation */
			break;
		case T_RowCompareExpr:
			/* RowCompareExpr's result is boolean ... */
			Assert(!OidIsValid(collation)); /* ... so never set a collation */
			break;
		case T_CoalesceExpr:
			((CoalesceExpr *) expr)->coalescecollid = collation;
			break;
		case T_MinMaxExpr:
			((MinMaxExpr *) expr)->minmaxcollid = collation;
			break;
		case T_SQLValueFunction:
			Assert((((SQLValueFunction *) expr)->type == NAMEOID) ?
				   (collation == C_COLLATION_OID) :
				   (collation == InvalidOid));
			break;
		case T_XmlExpr:
			Assert((((XmlExpr *) expr)->op == IS_XMLSERIALIZE) ?
				   (collation == DEFAULT_COLLATION_OID) :
				   (collation == InvalidOid));
			break;
		case T_JsonValueExpr:
			exprSetCollation((Node *) ((JsonValueExpr *) expr)->formatted_expr,
							 collation);
			break;
		case T_JsonConstructorExpr:
			{
				JsonConstructorExpr *ctor = (JsonConstructorExpr *) expr;

				if (ctor->coercion)
					exprSetCollation((Node *) ctor->coercion, collation);
				else
					Assert(!OidIsValid(collation)); /* result is always a
													 * json[b] type */
			}
			break;
		case T_JsonIsPredicate:
			Assert(!OidIsValid(collation)); /* result is always boolean */
			break;
		case T_JsonExpr:
			{
				JsonExpr   *jexpr = (JsonExpr *) expr;

				jexpr->collation = collation;
			}
			break;
		case T_JsonBehavior:
			{
				JsonBehavior *behavior = (JsonBehavior *) expr;

				if (behavior->expr)
					exprSetCollation(behavior->expr, collation);
			}
			break;
		case T_NullTest:
			/* NullTest's result is boolean ... */
			Assert(!OidIsValid(collation)); /* ... so never set a collation */
			break;
		case T_BooleanTest:
			/* BooleanTest's result is boolean ... */
			Assert(!OidIsValid(collation)); /* ... so never set a collation */
			break;
		case T_CoerceToDomain:
			((CoerceToDomain *) expr)->resultcollid = collation;
			break;
		case T_CoerceToDomainValue:
			((CoerceToDomainValue *) expr)->collation = collation;
			break;
		case T_SetToDefault:
			((SetToDefault *) expr)->collation = collation;
			break;
		case T_CurrentOfExpr:
			/* CurrentOfExpr's result is boolean ... */
			Assert(!OidIsValid(collation)); /* ... so never set a collation */
			break;
		case T_NextValueExpr:
			/* NextValueExpr's result is an integer type ... */
			Assert(!OidIsValid(collation)); /* ... so never set a collation */
			break;
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(expr));
			break;
	}
}

/*
 *	exprSetInputCollation -
 *	  Assign input-collation information to an expression tree node.
 *
 * This is a no-op for node types that don't store their input collation.
 * Note we omit RowCompareExpr, which needs special treatment since it
 * contains multiple input collation OIDs.
 */
void
exprSetInputCollation(Node *expr, Oid inputcollation)
{
	switch (nodeTag(expr))
	{
		case T_Aggref:
			((Aggref *) expr)->inputcollid = inputcollation;
			break;
		case T_WindowFunc:
			((WindowFunc *) expr)->inputcollid = inputcollation;
			break;
		case T_FuncExpr:
			((FuncExpr *) expr)->inputcollid = inputcollation;
			break;
		case T_OpExpr:
			((OpExpr *) expr)->inputcollid = inputcollation;
			break;
		case T_DistinctExpr:
			((DistinctExpr *) expr)->inputcollid = inputcollation;
			break;
		case T_NullIfExpr:
			((NullIfExpr *) expr)->inputcollid = inputcollation;
			break;
		case T_ScalarArrayOpExpr:
			((ScalarArrayOpExpr *) expr)->inputcollid = inputcollation;
			break;
		case T_MinMaxExpr:
			((MinMaxExpr *) expr)->inputcollid = inputcollation;
			break;
		default:
			break;
	}
}


/*
 *	exprLocation -
 *	  returns the parse location of an expression tree, for error reports
 *
 * -1 is returned if the location can't be determined.
 *
 * For expressions larger than a single token, the intent here is to
 * return the location of the expression's leftmost token, not necessarily
 * the topmost Node's location field.  For example, an OpExpr's location
 * field will point at the operator name, but if it is not a prefix operator
 * then we should return the location of the left-hand operand instead.
 * The reason is that we want to reference the entire expression not just
 * that operator, and pointing to its start seems to be the most natural way.
 *
 * The location is not perfect --- for example, since the grammar doesn't
 * explicitly represent parentheses in the parsetree, given something that
 * had been written "(a + b) * c" we are going to point at "a" not "(".
 * But it should be plenty good enough for error reporting purposes.
 *
 * You might think that this code is overly general, for instance why check
 * the operands of a FuncExpr node, when the function name can be expected
 * to be to the left of them?  There are a couple of reasons.  The grammar
 * sometimes builds expressions that aren't quite what the user wrote;
 * for instance x IS NOT BETWEEN ... becomes a NOT-expression whose keyword
 * pointer is to the right of its leftmost argument.  Also, nodes that were
 * inserted implicitly by parse analysis (such as FuncExprs for implicit
 * coercions) will have location -1, and so we can have odd combinations of
 * known and unknown locations in a tree.
 */
int
exprLocation(const Node *expr)
{
	int			loc;

	if (expr == NULL)
		return -1;
	switch (nodeTag(expr))
	{
		case T_RangeVar:
			loc = ((const RangeVar *) expr)->location;
			break;
		case T_TableFunc:
			loc = ((const TableFunc *) expr)->location;
			break;
		case T_Var:
			loc = ((const Var *) expr)->location;
			break;
		case T_Const:
			loc = ((const Const *) expr)->location;
			break;
		case T_Param:
			loc = ((const Param *) expr)->location;
			break;
		case T_Aggref:
			/* function name should always be the first thing */
			loc = ((const Aggref *) expr)->location;
			break;
		case T_GroupingFunc:
			loc = ((const GroupingFunc *) expr)->location;
			break;
		case T_WindowFunc:
			/* function name should always be the first thing */
			loc = ((const WindowFunc *) expr)->location;
			break;
		case T_MergeSupportFunc:
			loc = ((const MergeSupportFunc *) expr)->location;
			break;
		case T_SubscriptingRef:
			/* just use container argument's location */
			loc = exprLocation((Node *) ((const SubscriptingRef *) expr)->refexpr);
			break;
		case T_FuncExpr:
			{
				const FuncExpr *fexpr = (const FuncExpr *) expr;

				/* consider both function name and leftmost arg */
				loc = leftmostLoc(fexpr->location,
								  exprLocation((Node *) fexpr->args));
			}
			break;
		case T_NamedArgExpr:
			{
				const NamedArgExpr *na = (const NamedArgExpr *) expr;

				/* consider both argument name and value */
				loc = leftmostLoc(na->location,
								  exprLocation((Node *) na->arg));
			}
			break;
		case T_OpExpr:
		case T_DistinctExpr:	/* struct-equivalent to OpExpr */
		case T_NullIfExpr:		/* struct-equivalent to OpExpr */
			{
				const OpExpr *opexpr = (const OpExpr *) expr;

				/* consider both operator name and leftmost arg */
				loc = leftmostLoc(opexpr->location,
								  exprLocation((Node *) opexpr->args));
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				const ScalarArrayOpExpr *saopexpr = (const ScalarArrayOpExpr *) expr;

				/* consider both operator name and leftmost arg */
				loc = leftmostLoc(saopexpr->location,
								  exprLocation((Node *) saopexpr->args));
			}
			break;
		case T_BoolExpr:
			{
				const BoolExpr *bexpr = (const BoolExpr *) expr;

				/*
				 * Same as above, to handle either NOT or AND/OR.  We can't
				 * special-case NOT because of the way that it's used for
				 * things like IS NOT BETWEEN.
				 */
				loc = leftmostLoc(bexpr->location,
								  exprLocation((Node *) bexpr->args));
			}
			break;
		case T_SubLink:
			{
				const SubLink *sublink = (const SubLink *) expr;

				/* check the testexpr, if any, and the operator/keyword */
				loc = leftmostLoc(exprLocation(sublink->testexpr),
								  sublink->location);
			}
			break;
		case T_FieldSelect:
			/* just use argument's location */
			loc = exprLocation((Node *) ((const FieldSelect *) expr)->arg);
			break;
		case T_FieldStore:
			/* just use argument's location */
			loc = exprLocation((Node *) ((const FieldStore *) expr)->arg);
			break;
		case T_RelabelType:
			{
				const RelabelType *rexpr = (const RelabelType *) expr;

				/* Much as above */
				loc = leftmostLoc(rexpr->location,
								  exprLocation((Node *) rexpr->arg));
			}
			break;
		case T_CoerceViaIO:
			{
				const CoerceViaIO *cexpr = (const CoerceViaIO *) expr;

				/* Much as above */
				loc = leftmostLoc(cexpr->location,
								  exprLocation((Node *) cexpr->arg));
			}
			break;
		case T_ArrayCoerceExpr:
			{
				const ArrayCoerceExpr *cexpr = (const ArrayCoerceExpr *) expr;

				/* Much as above */
				loc = leftmostLoc(cexpr->location,
								  exprLocation((Node *) cexpr->arg));
			}
			break;
		case T_ConvertRowtypeExpr:
			{
				const ConvertRowtypeExpr *cexpr = (const ConvertRowtypeExpr *) expr;

				/* Much as above */
				loc = leftmostLoc(cexpr->location,
								  exprLocation((Node *) cexpr->arg));
			}
			break;
		case T_CollateExpr:
			/* just use argument's location */
			loc = exprLocation((Node *) ((const CollateExpr *) expr)->arg);
			break;
		case T_CaseExpr:
			/* CASE keyword should always be the first thing */
			loc = ((const CaseExpr *) expr)->location;
			break;
		case T_CaseWhen:
			/* WHEN keyword should always be the first thing */
			loc = ((const CaseWhen *) expr)->location;
			break;
		case T_ArrayExpr:
			/* the location points at ARRAY or [, which must be leftmost */
			loc = ((const ArrayExpr *) expr)->location;
			break;
		case T_RowExpr:
			/* the location points at ROW or (, which must be leftmost */
			loc = ((const RowExpr *) expr)->location;
			break;
		case T_RowCompareExpr:
			/* just use leftmost argument's location */
			loc = exprLocation((Node *) ((const RowCompareExpr *) expr)->largs);
			break;
		case T_CoalesceExpr:
			/* COALESCE keyword should always be the first thing */
			loc = ((const CoalesceExpr *) expr)->location;
			break;
		case T_MinMaxExpr:
			/* GREATEST/LEAST keyword should always be the first thing */
			loc = ((const MinMaxExpr *) expr)->location;
			break;
		case T_SQLValueFunction:
			/* function keyword should always be the first thing */
			loc = ((const SQLValueFunction *) expr)->location;
			break;
		case T_XmlExpr:
			{
				const XmlExpr *xexpr = (const XmlExpr *) expr;

				/* consider both function name and leftmost arg */
				loc = leftmostLoc(xexpr->location,
								  exprLocation((Node *) xexpr->args));
			}
			break;
		case T_JsonFormat:
			loc = ((const JsonFormat *) expr)->location;
			break;
		case T_JsonValueExpr:
			loc = exprLocation((Node *) ((const JsonValueExpr *) expr)->raw_expr);
			break;
		case T_JsonConstructorExpr:
			loc = ((const JsonConstructorExpr *) expr)->location;
			break;
		case T_JsonIsPredicate:
			loc = ((const JsonIsPredicate *) expr)->location;
			break;
		case T_JsonExpr:
			{
				const JsonExpr *jsexpr = (const JsonExpr *) expr;

				/* consider both function name and leftmost arg */
				loc = leftmostLoc(jsexpr->location,
								  exprLocation(jsexpr->formatted_expr));
			}
			break;
		case T_JsonBehavior:
			loc = exprLocation(((JsonBehavior *) expr)->expr);
			break;
		case T_NullTest:
			{
				const NullTest *nexpr = (const NullTest *) expr;

				/* Much as above */
				loc = leftmostLoc(nexpr->location,
								  exprLocation((Node *) nexpr->arg));
			}
			break;
		case T_BooleanTest:
			{
				const BooleanTest *bexpr = (const BooleanTest *) expr;

				/* Much as above */
				loc = leftmostLoc(bexpr->location,
								  exprLocation((Node *) bexpr->arg));
			}
			break;
		case T_CoerceToDomain:
			{
				const CoerceToDomain *cexpr = (const CoerceToDomain *) expr;

				/* Much as above */
				loc = leftmostLoc(cexpr->location,
								  exprLocation((Node *) cexpr->arg));
			}
			break;
		case T_CoerceToDomainValue:
			loc = ((const CoerceToDomainValue *) expr)->location;
			break;
		case T_SetToDefault:
			loc = ((const SetToDefault *) expr)->location;
			break;
		case T_TargetEntry:
			/* just use argument's location */
			loc = exprLocation((Node *) ((const TargetEntry *) expr)->expr);
			break;
		case T_IntoClause:
			/* use the contained RangeVar's location --- close enough */
			loc = exprLocation((Node *) ((const IntoClause *) expr)->rel);
			break;
		case T_List:
			{
				/* report location of first list member that has a location */
				ListCell   *lc;

				loc = -1;		/* just to suppress compiler warning */
				foreach(lc, (const List *) expr)
				{
					loc = exprLocation((Node *) lfirst(lc));
					if (loc >= 0)
						break;
				}
			}
			break;
		case T_A_Expr:
			{
				const A_Expr *aexpr = (const A_Expr *) expr;

				/* use leftmost of operator or left operand (if any) */
				/* we assume right operand can't be to left of operator */
				loc = leftmostLoc(aexpr->location,
								  exprLocation(aexpr->lexpr));
			}
			break;
		case T_ColumnRef:
			loc = ((const ColumnRef *) expr)->location;
			break;
		case T_ParamRef:
			loc = ((const ParamRef *) expr)->location;
			break;
		case T_A_Const:
			loc = ((const A_Const *) expr)->location;
			break;
		case T_FuncCall:
			{
				const FuncCall *fc = (const FuncCall *) expr;

				/* consider both function name and leftmost arg */
				/* (we assume any ORDER BY nodes must be to right of name) */
				loc = leftmostLoc(fc->location,
								  exprLocation((Node *) fc->args));
			}
			break;
		case T_A_ArrayExpr:
			/* the location points at ARRAY or [, which must be leftmost */
			loc = ((const A_ArrayExpr *) expr)->location;
			break;
		case T_ResTarget:
			/* we need not examine the contained expression (if any) */
			loc = ((const ResTarget *) expr)->location;
			break;
		case T_MultiAssignRef:
			loc = exprLocation(((const MultiAssignRef *) expr)->source);
			break;
		case T_TypeCast:
			{
				const TypeCast *tc = (const TypeCast *) expr;

				/*
				 * This could represent CAST(), ::, or TypeName 'literal', so
				 * any of the components might be leftmost.
				 */
				loc = exprLocation(tc->arg);
				loc = leftmostLoc(loc, tc->typeName->location);
				loc = leftmostLoc(loc, tc->location);
			}
			break;
		case T_CollateClause:
			/* just use argument's location */
			loc = exprLocation(((const CollateClause *) expr)->arg);
			break;
		case T_SortBy:
			/* just use argument's location (ignore operator, if any) */
			loc = exprLocation(((const SortBy *) expr)->node);
			break;
		case T_WindowDef:
			loc = ((const WindowDef *) expr)->location;
			break;
		case T_RangeTableSample:
			loc = ((const RangeTableSample *) expr)->location;
			break;
		case T_TypeName:
			loc = ((const TypeName *) expr)->location;
			break;
		case T_ColumnDef:
			loc = ((const ColumnDef *) expr)->location;
			break;
		case T_Constraint:
			loc = ((const Constraint *) expr)->location;
			break;
		case T_FunctionParameter:
			loc = ((const FunctionParameter *) expr)->location;
			break;
		case T_XmlSerialize:
			/* XMLSERIALIZE keyword should always be the first thing */
			loc = ((const XmlSerialize *) expr)->location;
			break;
		case T_GroupingSet:
			loc = ((const GroupingSet *) expr)->location;
			break;
		case T_WithClause:
			loc = ((const WithClause *) expr)->location;
			break;
		case T_InferClause:
			loc = ((const InferClause *) expr)->location;
			break;
		case T_OnConflictClause:
			loc = ((const OnConflictClause *) expr)->location;
			break;
		case T_CTESearchClause:
			loc = ((const CTESearchClause *) expr)->location;
			break;
		case T_CTECycleClause:
			loc = ((const CTECycleClause *) expr)->location;
			break;
		case T_CommonTableExpr:
			loc = ((const CommonTableExpr *) expr)->location;
			break;
		case T_JsonKeyValue:
			/* just use the key's location */
			loc = exprLocation((Node *) ((const JsonKeyValue *) expr)->key);
			break;
		case T_JsonObjectConstructor:
			loc = ((const JsonObjectConstructor *) expr)->location;
			break;
		case T_JsonArrayConstructor:
			loc = ((const JsonArrayConstructor *) expr)->location;
			break;
		case T_JsonArrayQueryConstructor:
			loc = ((const JsonArrayQueryConstructor *) expr)->location;
			break;
		case T_JsonAggConstructor:
			loc = ((const JsonAggConstructor *) expr)->location;
			break;
		case T_JsonObjectAgg:
			loc = exprLocation((Node *) ((const JsonObjectAgg *) expr)->constructor);
			break;
		case T_JsonArrayAgg:
			loc = exprLocation((Node *) ((const JsonArrayAgg *) expr)->constructor);
			break;
		case T_PlaceHolderVar:
			/* just use argument's location */
			loc = exprLocation((Node *) ((const PlaceHolderVar *) expr)->phexpr);
			break;
		case T_InferenceElem:
			/* just use nested expr's location */
			loc = exprLocation((Node *) ((const InferenceElem *) expr)->expr);
			break;
		case T_PartitionElem:
			loc = ((const PartitionElem *) expr)->location;
			break;
		case T_PartitionSpec:
			loc = ((const PartitionSpec *) expr)->location;
			break;
		case T_PartitionBoundSpec:
			loc = ((const PartitionBoundSpec *) expr)->location;
			break;
		case T_PartitionRangeDatum:
			loc = ((const PartitionRangeDatum *) expr)->location;
			break;
		default:
			/* for any other node type it's just unknown... */
			loc = -1;
			break;
	}
	return loc;
}

/*
 * leftmostLoc - support for exprLocation
 *
 * Take the minimum of two parse location values, but ignore unknowns
 */
static int
leftmostLoc(int loc1, int loc2)
{
	if (loc1 < 0)
		return loc2;
	else if (loc2 < 0)
		return loc1;
	else
		return Min(loc1, loc2);
}


/*
 * fix_opfuncids
 *	  Calculate opfuncid field from opno for each OpExpr node in given tree.
 *	  The given tree can be anything expression_tree_walker handles.
 *
 * The argument is modified in-place.  (This is OK since we'd want the
 * same change for any node, even if it gets visited more than once due to
 * shared structure.)
 */
void
fix_opfuncids(Node *node)
{
	/* This tree walk requires no special setup, so away we go... */
	fix_opfuncids_walker(node, NULL);
}

static bool
fix_opfuncids_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, OpExpr))
		set_opfuncid((OpExpr *) node);
	else if (IsA(node, DistinctExpr))
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
	else if (IsA(node, NullIfExpr))
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
	else if (IsA(node, ScalarArrayOpExpr))
		set_sa_opfuncid((ScalarArrayOpExpr *) node);
	return expression_tree_walker(node, fix_opfuncids_walker, context);
}

/*
 * set_opfuncid
 *		Set the opfuncid (procedure OID) in an OpExpr node,
 *		if it hasn't been set already.
 *
 * Because of struct equivalence, this can also be used for
 * DistinctExpr and NullIfExpr nodes.
 */
void
set_opfuncid(OpExpr *opexpr)
{
	if (opexpr->opfuncid == InvalidOid)
		opexpr->opfuncid = get_opcode(opexpr->opno);
}

/*
 * set_sa_opfuncid
 *		As above, for ScalarArrayOpExpr nodes.
 */
void
set_sa_opfuncid(ScalarArrayOpExpr *opexpr)
{
	if (opexpr->opfuncid == InvalidOid)
		opexpr->opfuncid = get_opcode(opexpr->opno);
}


/*
 *	check_functions_in_node -
 *	  apply checker() to each function OID contained in given expression node
 *
 * Returns true if the checker() function does; for nodes representing more
 * than one function call, returns true if the checker() function does so
 * for any of those functions.  Returns false if node does not invoke any
 * SQL-visible function.  Caller must not pass node == NULL.
 *
 * This function examines only the given node; it does not recurse into any
 * sub-expressions.  Callers typically prefer to keep control of the recursion
 * for themselves, in case additional checks should be made, or because they
 * have special rules about which parts of the tree need to be visited.
 *
 * Note: we ignore MinMaxExpr, SQLValueFunction, XmlExpr, CoerceToDomain,
 * and NextValueExpr nodes, because they do not contain SQL function OIDs.
 * However, they can invoke SQL-visible functions, so callers should take
 * thought about how to treat them.
 */
bool
check_functions_in_node(Node *node, check_function_callback checker,
						void *context)
{
	switch (nodeTag(node))
	{
		case T_Aggref:
			{
				Aggref	   *expr = (Aggref *) node;

				if (checker(expr->aggfnoid, context))
					return true;
			}
			break;
		case T_WindowFunc:
			{
				WindowFunc *expr = (WindowFunc *) node;

				if (checker(expr->winfnoid, context))
					return true;
			}
			break;
		case T_FuncExpr:
			{
				FuncExpr   *expr = (FuncExpr *) node;

				if (checker(expr->funcid, context))
					return true;
			}
			break;
		case T_OpExpr:
		case T_DistinctExpr:	/* struct-equivalent to OpExpr */
		case T_NullIfExpr:		/* struct-equivalent to OpExpr */
			{
				OpExpr	   *expr = (OpExpr *) node;

				/* Set opfuncid if it wasn't set already */
				set_opfuncid(expr);
				if (checker(expr->opfuncid, context))
					return true;
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;

				set_sa_opfuncid(expr);
				if (checker(expr->opfuncid, context))
					return true;
			}
			break;
		case T_CoerceViaIO:
			{
				CoerceViaIO *expr = (CoerceViaIO *) node;
				Oid			iofunc;
				Oid			typioparam;
				bool		typisvarlena;

				/* check the result type's input function */
				getTypeInputInfo(expr->resulttype,
								 &iofunc, &typioparam);
				if (checker(iofunc, context))
					return true;
				/* check the input type's output function */
				getTypeOutputInfo(exprType((Node *) expr->arg),
								  &iofunc, &typisvarlena);
				if (checker(iofunc, context))
					return true;
			}
			break;
		case T_RowCompareExpr:
			{
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;
				ListCell   *opid;

				foreach(opid, rcexpr->opnos)
				{
					Oid			opfuncid = get_opcode(lfirst_oid(opid));

					if (checker(opfuncid, context))
						return true;
				}
			}
			break;
		default:
			break;
	}
	return false;
}


/*
 * Standard expression-tree walking support
 *
 * We used to have near-duplicate code in many different routines that
 * understood how to recurse through an expression node tree.  That was
 * a pain to maintain, and we frequently had bugs due to some particular
 * routine neglecting to support a particular node type.  In most cases,
 * these routines only actually care about certain node types, and don't
 * care about other types except insofar as they have to recurse through
 * non-primitive node types.  Therefore, we now provide generic tree-walking
 * logic to consolidate the redundant "boilerplate" code.  There are
 * two versions: expression_tree_walker() and expression_tree_mutator().
 */

/*
 * expression_tree_walker() is designed to support routines that traverse
 * a tree in a read-only fashion (although it will also work for routines
 * that modify nodes in-place but never add/delete/replace nodes).
 * A walker routine should look like this:
 *
 * bool my_walker (Node *node, my_struct *context)
 * {
 *		if (node == NULL)
 *			return false;
 *		// check for nodes that special work is required for, eg:
 *		if (IsA(node, Var))
 *		{
 *			... do special actions for Var nodes
 *		}
 *		else if (IsA(node, ...))
 *		{
 *			... do special actions for other node types
 *		}
 *		// for any node type not specially processed, do:
 *		return expression_tree_walker(node, my_walker, (void *) context);
 * }
 *
 * The "context" argument points to a struct that holds whatever context
 * information the walker routine needs --- it can be used to return data
 * gathered by the walker, too.  This argument is not touched by
 * expression_tree_walker, but it is passed down to recursive sub-invocations
 * of my_walker.  The tree walk is started from a setup routine that
 * fills in the appropriate context struct, calls my_walker with the top-level
 * node of the tree, and then examines the results.
 *
 * The walker routine should return "false" to continue the tree walk, or
 * "true" to abort the walk and immediately return "true" to the top-level
 * caller.  This can be used to short-circuit the traversal if the walker
 * has found what it came for.  "false" is returned to the top-level caller
 * iff no invocation of the walker returned "true".
 *
 * The node types handled by expression_tree_walker include all those
 * normally found in target lists and qualifier clauses during the planning
 * stage.  In particular, it handles List nodes since a cnf-ified qual clause
 * will have List structure at the top level, and it handles TargetEntry nodes
 * so that a scan of a target list can be handled without additional code.
 * Also, RangeTblRef, FromExpr, JoinExpr, and SetOperationStmt nodes are
 * handled, so that query jointrees and setOperation trees can be processed
 * without additional code.
 *
 * expression_tree_walker will handle SubLink nodes by recursing normally
 * into the "testexpr" subtree (which is an expression belonging to the outer
 * plan).  It will also call the walker on the sub-Query node; however, when
 * expression_tree_walker itself is called on a Query node, it does nothing
 * and returns "false".  The net effect is that unless the walker does
 * something special at a Query node, sub-selects will not be visited during
 * an expression tree walk. This is exactly the behavior wanted in many cases
 * --- and for those walkers that do want to recurse into sub-selects, special
 * behavior is typically needed anyway at the entry to a sub-select (such as
 * incrementing a depth counter). A walker that wants to examine sub-selects
 * should include code along the lines of:
 *
 *		if (IsA(node, Query))
 *		{
 *			adjust context for subquery;
 *			result = query_tree_walker((Query *) node, my_walker, context,
 *									   0); // adjust flags as needed
 *			restore context if needed;
 *			return result;
 *		}
 *
 * query_tree_walker is a convenience routine (see below) that calls the
 * walker on all the expression subtrees of the given Query node.
 *
 * expression_tree_walker will handle SubPlan nodes by recursing normally
 * into the "testexpr" and the "args" list (which are expressions belonging to
 * the outer plan).  It will not touch the completed subplan, however.  Since
 * there is no link to the original Query, it is not possible to recurse into
 * subselects of an already-planned expression tree.  This is OK for current
 * uses, but may need to be revisited in future.
 */

bool
expression_tree_walker_impl(Node *node,
							tree_walker_callback walker,
							void *context)
{
	ListCell   *temp;

	/*
	 * The walker has already visited the current node, and so we need only
	 * recurse into any sub-nodes it has.
	 *
	 * We assume that the walker is not interested in List nodes per se, so
	 * when we expect a List we just recurse directly to self without
	 * bothering to call the walker.
	 */
#define WALK(n) walker((Node *) (n), context)

#define LIST_WALK(l) expression_tree_walker_impl((Node *) (l), walker, context)

	if (node == NULL)
		return false;

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	switch (nodeTag(node))
	{
		case T_Var:
		case T_Const:
		case T_Param:
		case T_CaseTestExpr:
		case T_SQLValueFunction:
		case T_CoerceToDomainValue:
		case T_SetToDefault:
		case T_CurrentOfExpr:
		case T_NextValueExpr:
		case T_RangeTblRef:
		case T_SortGroupClause:
		case T_CTESearchClause:
		case T_MergeSupportFunc:
			/* primitive node types with no expression subnodes */
			break;
		case T_WithCheckOption:
			return WALK(((WithCheckOption *) node)->qual);
		case T_Aggref:
			{
				Aggref	   *expr = (Aggref *) node;

				/* recurse directly on Lists */
				if (LIST_WALK(expr->aggdirectargs))
					return true;
				if (LIST_WALK(expr->args))
					return true;
				if (LIST_WALK(expr->aggorder))
					return true;
				if (LIST_WALK(expr->aggdistinct))
					return true;
				if (WALK(expr->aggfilter))
					return true;
			}
			break;
		case T_GroupingFunc:
			{
				GroupingFunc *grouping = (GroupingFunc *) node;

				if (LIST_WALK(grouping->args))
					return true;
			}
			break;
		case T_WindowFunc:
			{
				WindowFunc *expr = (WindowFunc *) node;

				/* recurse directly on List */
				if (LIST_WALK(expr->args))
					return true;
				if (WALK(expr->aggfilter))
					return true;
				if (WALK(expr->runCondition))
					return true;
			}
			break;
		case T_WindowFuncRunCondition:
			{
				WindowFuncRunCondition *expr = (WindowFuncRunCondition *) node;

				if (WALK(expr->arg))
					return true;
			}
			break;
		case T_SubscriptingRef:
			{
				SubscriptingRef *sbsref = (SubscriptingRef *) node;

				/* recurse directly for upper/lower container index lists */
				if (LIST_WALK(sbsref->refupperindexpr))
					return true;
				if (LIST_WALK(sbsref->reflowerindexpr))
					return true;
				/* walker must see the refexpr and refassgnexpr, however */
				if (WALK(sbsref->refexpr))
					return true;

				if (WALK(sbsref->refassgnexpr))
					return true;
			}
			break;
		case T_FuncExpr:
			{
				FuncExpr   *expr = (FuncExpr *) node;

				if (LIST_WALK(expr->args))
					return true;
			}
			break;
		case T_NamedArgExpr:
			return WALK(((NamedArgExpr *) node)->arg);
		case T_OpExpr:
		case T_DistinctExpr:	/* struct-equivalent to OpExpr */
		case T_NullIfExpr:		/* struct-equivalent to OpExpr */
			{
				OpExpr	   *expr = (OpExpr *) node;

				if (LIST_WALK(expr->args))
					return true;
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;

				if (LIST_WALK(expr->args))
					return true;
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;

				if (LIST_WALK(expr->args))
					return true;
			}
			break;
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;

				if (WALK(sublink->testexpr))
					return true;

				/*
				 * Also invoke the walker on the sublink's Query node, so it
				 * can recurse into the sub-query if it wants to.
				 */
				return WALK(sublink->subselect);
			}
			break;
		case T_SubPlan:
			{
				SubPlan    *subplan = (SubPlan *) node;

				/* recurse into the testexpr, but not into the Plan */
				if (WALK(subplan->testexpr))
					return true;
				/* also examine args list */
				if (LIST_WALK(subplan->args))
					return true;
			}
			break;
		case T_AlternativeSubPlan:
			return LIST_WALK(((AlternativeSubPlan *) node)->subplans);
		case T_FieldSelect:
			return WALK(((FieldSelect *) node)->arg);
		case T_FieldStore:
			{
				FieldStore *fstore = (FieldStore *) node;

				if (WALK(fstore->arg))
					return true;
				if (WALK(fstore->newvals))
					return true;
			}
			break;
		case T_RelabelType:
			return WALK(((RelabelType *) node)->arg);
		case T_CoerceViaIO:
			return WALK(((CoerceViaIO *) node)->arg);
		case T_ArrayCoerceExpr:
			{
				ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) node;

				if (WALK(acoerce->arg))
					return true;
				if (WALK(acoerce->elemexpr))
					return true;
			}
			break;
		case T_ConvertRowtypeExpr:
			return WALK(((ConvertRowtypeExpr *) node)->arg);
		case T_CollateExpr:
			return WALK(((CollateExpr *) node)->arg);
		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;

				if (WALK(caseexpr->arg))
					return true;
				/* we assume walker doesn't care about CaseWhens, either */
				foreach(temp, caseexpr->args)
				{
					CaseWhen   *when = lfirst_node(CaseWhen, temp);

					if (WALK(when->expr))
						return true;
					if (WALK(when->result))
						return true;
				}
				if (WALK(caseexpr->defresult))
					return true;
			}
			break;
		case T_ArrayExpr:
			return WALK(((ArrayExpr *) node)->elements);
		case T_RowExpr:
			/* Assume colnames isn't interesting */
			return WALK(((RowExpr *) node)->args);
		case T_RowCompareExpr:
			{
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;

				if (WALK(rcexpr->largs))
					return true;
				if (WALK(rcexpr->rargs))
					return true;
			}
			break;
		case T_CoalesceExpr:
			return WALK(((CoalesceExpr *) node)->args);
		case T_MinMaxExpr:
			return WALK(((MinMaxExpr *) node)->args);
		case T_XmlExpr:
			{
				XmlExpr    *xexpr = (XmlExpr *) node;

				if (WALK(xexpr->named_args))
					return true;
				/* we assume walker doesn't care about arg_names */
				if (WALK(xexpr->args))
					return true;
			}
			break;
		case T_JsonValueExpr:
			{
				JsonValueExpr *jve = (JsonValueExpr *) node;

				if (WALK(jve->raw_expr))
					return true;
				if (WALK(jve->formatted_expr))
					return true;
			}
			break;
		case T_JsonConstructorExpr:
			{
				JsonConstructorExpr *ctor = (JsonConstructorExpr *) node;

				if (WALK(ctor->args))
					return true;
				if (WALK(ctor->func))
					return true;
				if (WALK(ctor->coercion))
					return true;
			}
			break;
		case T_JsonIsPredicate:
			return WALK(((JsonIsPredicate *) node)->expr);
		case T_JsonExpr:
			{
				JsonExpr   *jexpr = (JsonExpr *) node;

				if (WALK(jexpr->formatted_expr))
					return true;
				if (WALK(jexpr->path_spec))
					return true;
				if (WALK(jexpr->passing_values))
					return true;
				/* we assume walker doesn't care about passing_names */
				if (WALK(jexpr->on_empty))
					return true;
				if (WALK(jexpr->on_error))
					return true;
			}
			break;
		case T_JsonBehavior:
			{
				JsonBehavior *behavior = (JsonBehavior *) node;

				if (WALK(behavior->expr))
					return true;
			}
			break;
		case T_NullTest:
			return WALK(((NullTest *) node)->arg);
		case T_BooleanTest:
			return WALK(((BooleanTest *) node)->arg);
		case T_CoerceToDomain:
			return WALK(((CoerceToDomain *) node)->arg);
		case T_TargetEntry:
			return WALK(((TargetEntry *) node)->expr);
		case T_Query:
			/* Do nothing with a sub-Query, per discussion above */
			break;
		case T_WindowClause:
			{
				WindowClause *wc = (WindowClause *) node;

				if (WALK(wc->partitionClause))
					return true;
				if (WALK(wc->orderClause))
					return true;
				if (WALK(wc->startOffset))
					return true;
				if (WALK(wc->endOffset))
					return true;
			}
			break;
		case T_CTECycleClause:
			{
				CTECycleClause *cc = (CTECycleClause *) node;

				if (WALK(cc->cycle_mark_value))
					return true;
				if (WALK(cc->cycle_mark_default))
					return true;
			}
			break;
		case T_CommonTableExpr:
			{
				CommonTableExpr *cte = (CommonTableExpr *) node;

				/*
				 * Invoke the walker on the CTE's Query node, so it can
				 * recurse into the sub-query if it wants to.
				 */
				if (WALK(cte->ctequery))
					return true;

				if (WALK(cte->search_clause))
					return true;
				if (WALK(cte->cycle_clause))
					return true;
			}
			break;
		case T_JsonKeyValue:
			{
				JsonKeyValue *kv = (JsonKeyValue *) node;

				if (WALK(kv->key))
					return true;
				if (WALK(kv->value))
					return true;
			}
			break;
		case T_JsonObjectConstructor:
			{
				JsonObjectConstructor *ctor = (JsonObjectConstructor *) node;

				if (LIST_WALK(ctor->exprs))
					return true;
			}
			break;
		case T_JsonArrayConstructor:
			{
				JsonArrayConstructor *ctor = (JsonArrayConstructor *) node;

				if (LIST_WALK(ctor->exprs))
					return true;
			}
			break;
		case T_JsonArrayQueryConstructor:
			{
				JsonArrayQueryConstructor *ctor = (JsonArrayQueryConstructor *) node;

				if (WALK(ctor->query))
					return true;
			}
			break;
		case T_JsonAggConstructor:
			{
				JsonAggConstructor *ctor = (JsonAggConstructor *) node;

				if (WALK(ctor->agg_filter))
					return true;
				if (WALK(ctor->agg_order))
					return true;
				if (WALK(ctor->over))
					return true;
			}
			break;
		case T_JsonObjectAgg:
			{
				JsonObjectAgg *ctor = (JsonObjectAgg *) node;

				if (WALK(ctor->constructor))
					return true;
				if (WALK(ctor->arg))
					return true;
			}
			break;
		case T_JsonArrayAgg:
			{
				JsonArrayAgg *ctor = (JsonArrayAgg *) node;

				if (WALK(ctor->constructor))
					return true;
				if (WALK(ctor->arg))
					return true;
			}
			break;

		case T_PartitionBoundSpec:
			{
				PartitionBoundSpec *pbs = (PartitionBoundSpec *) node;

				if (WALK(pbs->listdatums))
					return true;
				if (WALK(pbs->lowerdatums))
					return true;
				if (WALK(pbs->upperdatums))
					return true;
			}
			break;
		case T_PartitionRangeDatum:
			{
				PartitionRangeDatum *prd = (PartitionRangeDatum *) node;

				if (WALK(prd->value))
					return true;
			}
			break;
		case T_List:
			foreach(temp, (List *) node)
			{
				if (WALK(lfirst(temp)))
					return true;
			}
			break;
		case T_FromExpr:
			{
				FromExpr   *from = (FromExpr *) node;

				if (LIST_WALK(from->fromlist))
					return true;
				if (WALK(from->quals))
					return true;
			}
			break;
		case T_OnConflictExpr:
			{
				OnConflictExpr *onconflict = (OnConflictExpr *) node;

				if (WALK(onconflict->arbiterElems))
					return true;
				if (WALK(onconflict->arbiterWhere))
					return true;
				if (WALK(onconflict->onConflictSet))
					return true;
				if (WALK(onconflict->onConflictWhere))
					return true;
				if (WALK(onconflict->exclRelTlist))
					return true;
			}
			break;
		case T_MergeAction:
			{
				MergeAction *action = (MergeAction *) node;

				if (WALK(action->qual))
					return true;
				if (WALK(action->targetList))
					return true;
			}
			break;
		case T_PartitionPruneStepOp:
			{
				PartitionPruneStepOp *opstep = (PartitionPruneStepOp *) node;

				if (WALK(opstep->exprs))
					return true;
			}
			break;
		case T_PartitionPruneStepCombine:
			/* no expression subnodes */
			break;
		case T_JoinExpr:
			{
				JoinExpr   *join = (JoinExpr *) node;

				if (WALK(join->larg))
					return true;
				if (WALK(join->rarg))
					return true;
				if (WALK(join->quals))
					return true;

				/*
				 * alias clause, using list are deemed uninteresting.
				 */
			}
			break;
		case T_SetOperationStmt:
			{
				SetOperationStmt *setop = (SetOperationStmt *) node;

				if (WALK(setop->larg))
					return true;
				if (WALK(setop->rarg))
					return true;

				/* groupClauses are deemed uninteresting */
			}
			break;
		case T_IndexClause:
			{
				IndexClause *iclause = (IndexClause *) node;

				if (WALK(iclause->rinfo))
					return true;
				if (LIST_WALK(iclause->indexquals))
					return true;
			}
			break;
		case T_PlaceHolderVar:
			return WALK(((PlaceHolderVar *) node)->phexpr);
		case T_InferenceElem:
			return WALK(((InferenceElem *) node)->expr);
		case T_AppendRelInfo:
			{
				AppendRelInfo *appinfo = (AppendRelInfo *) node;

				if (LIST_WALK(appinfo->translated_vars))
					return true;
			}
			break;
		case T_PlaceHolderInfo:
			return WALK(((PlaceHolderInfo *) node)->ph_var);
		case T_RangeTblFunction:
			return WALK(((RangeTblFunction *) node)->funcexpr);
		case T_TableSampleClause:
			{
				TableSampleClause *tsc = (TableSampleClause *) node;

				if (LIST_WALK(tsc->args))
					return true;
				if (WALK(tsc->repeatable))
					return true;
			}
			break;
		case T_TableFunc:
			{
				TableFunc  *tf = (TableFunc *) node;

				if (WALK(tf->ns_uris))
					return true;
				if (WALK(tf->docexpr))
					return true;
				if (WALK(tf->rowexpr))
					return true;
				if (WALK(tf->colexprs))
					return true;
				if (WALK(tf->coldefexprs))
					return true;
				if (WALK(tf->colvalexprs))
					return true;
				if (WALK(tf->passingvalexprs))
					return true;
			}
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(node));
			break;
	}
	return false;

	/* The WALK() macro can be re-used below, but LIST_WALK() not so much */
#undef LIST_WALK
}

/*
 * query_tree_walker --- initiate a walk of a Query's expressions
 *
 * This routine exists just to reduce the number of places that need to know
 * where all the expression subtrees of a Query are.  Note it can be used
 * for starting a walk at top level of a Query regardless of whether the
 * walker intends to descend into subqueries.  It is also useful for
 * descending into subqueries within a walker.
 *
 * Some callers want to suppress visitation of certain items in the sub-Query,
 * typically because they need to process them specially, or don't actually
 * want to recurse into subqueries.  This is supported by the flags argument,
 * which is the bitwise OR of flag values to add or suppress visitation of
 * indicated items.  (More flag bits may be added as needed.)
 */
bool
query_tree_walker_impl(Query *query,
					   tree_walker_callback walker,
					   void *context,
					   int flags)
{
	Assert(query != NULL && IsA(query, Query));

	/*
	 * We don't walk any utilityStmt here. However, we can't easily assert
	 * that it is absent, since there are at least two code paths by which
	 * action statements from CREATE RULE end up here, and NOTIFY is allowed
	 * in a rule action.
	 */

	if (WALK(query->targetList))
		return true;
	if (WALK(query->withCheckOptions))
		return true;
	if (WALK(query->onConflict))
		return true;
	if (WALK(query->mergeActionList))
		return true;
	if (WALK(query->mergeJoinCondition))
		return true;
	if (WALK(query->returningList))
		return true;
	if (WALK(query->jointree))
		return true;
	if (WALK(query->setOperations))
		return true;
	if (WALK(query->havingQual))
		return true;
	if (WALK(query->limitOffset))
		return true;
	if (WALK(query->limitCount))
		return true;

	/*
	 * Most callers aren't interested in SortGroupClause nodes since those
	 * don't contain actual expressions. However they do contain OIDs which
	 * may be needed by dependency walkers etc.
	 */
	if ((flags & QTW_EXAMINE_SORTGROUP))
	{
		if (WALK(query->groupClause))
			return true;
		if (WALK(query->windowClause))
			return true;
		if (WALK(query->sortClause))
			return true;
		if (WALK(query->distinctClause))
			return true;
	}
	else
	{
		/*
		 * But we need to walk the expressions under WindowClause nodes even
		 * if we're not interested in SortGroupClause nodes.
		 */
		ListCell   *lc;

		foreach(lc, query->windowClause)
		{
			WindowClause *wc = lfirst_node(WindowClause, lc);

			if (WALK(wc->startOffset))
				return true;
			if (WALK(wc->endOffset))
				return true;
		}
	}

	/*
	 * groupingSets and rowMarks are not walked:
	 *
	 * groupingSets contain only ressortgrouprefs (integers) which are
	 * meaningless without the corresponding groupClause or tlist.
	 * Accordingly, any walker that needs to care about them needs to handle
	 * them itself in its Query processing.
	 *
	 * rowMarks is not walked because it contains only rangetable indexes (and
	 * flags etc.) and therefore should be handled at Query level similarly.
	 */

	if (!(flags & QTW_IGNORE_CTE_SUBQUERIES))
	{
		if (WALK(query->cteList))
			return true;
	}
	if (!(flags & QTW_IGNORE_RANGE_TABLE))
	{
		if (range_table_walker(query->rtable, walker, context, flags))
			return true;
	}
	return false;
}

/*
 * range_table_walker is just the part of query_tree_walker that scans
 * a query's rangetable.  This is split out since it can be useful on
 * its own.
 */
bool
range_table_walker_impl(List *rtable,
						tree_walker_callback walker,
						void *context,
						int flags)
{
	ListCell   *rt;

	foreach(rt, rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, rt);

		if (range_table_entry_walker(rte, walker, context, flags))
			return true;
	}
	return false;
}

/*
 * Some callers even want to scan the expressions in individual RTEs.
 */
bool
range_table_entry_walker_impl(RangeTblEntry *rte,
							  tree_walker_callback walker,
							  void *context,
							  int flags)
{
	/*
	 * Walkers might need to examine the RTE node itself either before or
	 * after visiting its contents (or, conceivably, both).  Note that if you
	 * specify neither flag, the walker won't be called on the RTE at all.
	 */
	if (flags & QTW_EXAMINE_RTES_BEFORE)
		if (WALK(rte))
			return true;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			if (WALK(rte->tablesample))
				return true;
			break;
		case RTE_SUBQUERY:
			if (!(flags & QTW_IGNORE_RT_SUBQUERIES))
				if (WALK(rte->subquery))
					return true;
			break;
		case RTE_JOIN:
			if (!(flags & QTW_IGNORE_JOINALIASES))
				if (WALK(rte->joinaliasvars))
					return true;
			break;
		case RTE_FUNCTION:
			if (WALK(rte->functions))
				return true;
			break;
		case RTE_TABLEFUNC:
			if (WALK(rte->tablefunc))
				return true;
			break;
		case RTE_VALUES:
			if (WALK(rte->values_lists))
				return true;
			break;
		case RTE_CTE:
		case RTE_NAMEDTUPLESTORE:
		case RTE_RESULT:
			/* nothing to do */
			break;
		case RTE_GROUP:
			if (!(flags & QTW_IGNORE_GROUPEXPRS))
				if (WALK(rte->groupexprs))
					return true;
			break;
	}

	if (WALK(rte->securityQuals))
		return true;

	if (flags & QTW_EXAMINE_RTES_AFTER)
		if (WALK(rte))
			return true;

	return false;
}


/*
 * expression_tree_mutator() is designed to support routines that make a
 * modified copy of an expression tree, with some nodes being added,
 * removed, or replaced by new subtrees.  The original tree is (normally)
 * not changed.  Each recursion level is responsible for returning a copy of
 * (or appropriately modified substitute for) the subtree it is handed.
 * A mutator routine should look like this:
 *
 * Node * my_mutator (Node *node, my_struct *context)
 * {
 *		if (node == NULL)
 *			return NULL;
 *		// check for nodes that special work is required for, eg:
 *		if (IsA(node, Var))
 *		{
 *			... create and return modified copy of Var node
 *		}
 *		else if (IsA(node, ...))
 *		{
 *			... do special transformations of other node types
 *		}
 *		// for any node type not specially processed, do:
 *		return expression_tree_mutator(node, my_mutator, (void *) context);
 * }
 *
 * The "context" argument points to a struct that holds whatever context
 * information the mutator routine needs --- it can be used to return extra
 * data gathered by the mutator, too.  This argument is not touched by
 * expression_tree_mutator, but it is passed down to recursive sub-invocations
 * of my_mutator.  The tree walk is started from a setup routine that
 * fills in the appropriate context struct, calls my_mutator with the
 * top-level node of the tree, and does any required post-processing.
 *
 * Each level of recursion must return an appropriately modified Node.
 * If expression_tree_mutator() is called, it will make an exact copy
 * of the given Node, but invoke my_mutator() to copy the sub-node(s)
 * of that Node.  In this way, my_mutator() has full control over the
 * copying process but need not directly deal with expression trees
 * that it has no interest in.
 *
 * Just as for expression_tree_walker, the node types handled by
 * expression_tree_mutator include all those normally found in target lists
 * and qualifier clauses during the planning stage.
 *
 * expression_tree_mutator will handle SubLink nodes by recursing normally
 * into the "testexpr" subtree (which is an expression belonging to the outer
 * plan).  It will also call the mutator on the sub-Query node; however, when
 * expression_tree_mutator itself is called on a Query node, it does nothing
 * and returns the unmodified Query node.  The net effect is that unless the
 * mutator does something special at a Query node, sub-selects will not be
 * visited or modified; the original sub-select will be linked to by the new
 * SubLink node.  Mutators that want to descend into sub-selects will usually
 * do so by recognizing Query nodes and calling query_tree_mutator (below).
 *
 * expression_tree_mutator will handle a SubPlan node by recursing into the
 * "testexpr" and the "args" list (which belong to the outer plan), but it
 * will simply copy the link to the inner plan, since that's typically what
 * expression tree mutators want.  A mutator that wants to modify the subplan
 * can force appropriate behavior by recognizing SubPlan expression nodes
 * and doing the right thing.
 */

Node *
expression_tree_mutator_impl(Node *node,
							 tree_mutator_callback mutator,
							 void *context)
{
	/*
	 * The mutator has already decided not to modify the current node, but we
	 * must call the mutator for any sub-nodes.
	 */

#define FLATCOPY(newnode, node, nodetype)  \
	( (newnode) = (nodetype *) palloc(sizeof(nodetype)), \
	  memcpy((newnode), (node), sizeof(nodetype)) )

#define MUTATE(newfield, oldfield, fieldtype)  \
		( (newfield) = (fieldtype) mutator((Node *) (oldfield), context) )

	if (node == NULL)
		return NULL;

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	switch (nodeTag(node))
	{
			/*
			 * Primitive node types with no expression subnodes.  Var and
			 * Const are frequent enough to deserve special cases, the others
			 * we just use copyObject for.
			 */
		case T_Var:
			{
				Var		   *var = (Var *) node;
				Var		   *newnode;

				FLATCOPY(newnode, var, Var);
				/* Assume we need not copy the varnullingrels bitmapset */
				return (Node *) newnode;
			}
			break;
		case T_Const:
			{
				Const	   *oldnode = (Const *) node;
				Const	   *newnode;

				FLATCOPY(newnode, oldnode, Const);
				/* XXX we don't bother with datumCopy; should we? */
				return (Node *) newnode;
			}
			break;
		case T_Param:
		case T_CaseTestExpr:
		case T_SQLValueFunction:
		case T_JsonFormat:
		case T_CoerceToDomainValue:
		case T_SetToDefault:
		case T_CurrentOfExpr:
		case T_NextValueExpr:
		case T_RangeTblRef:
		case T_SortGroupClause:
		case T_CTESearchClause:
		case T_MergeSupportFunc:
			return copyObject(node);
		case T_WithCheckOption:
			{
				WithCheckOption *wco = (WithCheckOption *) node;
				WithCheckOption *newnode;

				FLATCOPY(newnode, wco, WithCheckOption);
				MUTATE(newnode->qual, wco->qual, Node *);
				return (Node *) newnode;
			}
		case T_Aggref:
			{
				Aggref	   *aggref = (Aggref *) node;
				Aggref	   *newnode;

				FLATCOPY(newnode, aggref, Aggref);
				/* assume mutation doesn't change types of arguments */
				newnode->aggargtypes = list_copy(aggref->aggargtypes);
				MUTATE(newnode->aggdirectargs, aggref->aggdirectargs, List *);
				MUTATE(newnode->args, aggref->args, List *);
				MUTATE(newnode->aggorder, aggref->aggorder, List *);
				MUTATE(newnode->aggdistinct, aggref->aggdistinct, List *);
				MUTATE(newnode->aggfilter, aggref->aggfilter, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_GroupingFunc:
			{
				GroupingFunc *grouping = (GroupingFunc *) node;
				GroupingFunc *newnode;

				FLATCOPY(newnode, grouping, GroupingFunc);
				MUTATE(newnode->args, grouping->args, List *);

				/*
				 * We assume here that mutating the arguments does not change
				 * the semantics, i.e. that the arguments are not mutated in a
				 * way that makes them semantically different from their
				 * previously matching expressions in the GROUP BY clause.
				 *
				 * If a mutator somehow wanted to do this, it would have to
				 * handle the refs and cols lists itself as appropriate.
				 */
				newnode->refs = list_copy(grouping->refs);
				newnode->cols = list_copy(grouping->cols);

				return (Node *) newnode;
			}
			break;
		case T_WindowFunc:
			{
				WindowFunc *wfunc = (WindowFunc *) node;
				WindowFunc *newnode;

				FLATCOPY(newnode, wfunc, WindowFunc);
				MUTATE(newnode->args, wfunc->args, List *);
				MUTATE(newnode->aggfilter, wfunc->aggfilter, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_WindowFuncRunCondition:
			{
				WindowFuncRunCondition *wfuncrc = (WindowFuncRunCondition *) node;
				WindowFuncRunCondition *newnode;

				FLATCOPY(newnode, wfuncrc, WindowFuncRunCondition);
				MUTATE(newnode->arg, wfuncrc->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_SubscriptingRef:
			{
				SubscriptingRef *sbsref = (SubscriptingRef *) node;
				SubscriptingRef *newnode;

				FLATCOPY(newnode, sbsref, SubscriptingRef);
				MUTATE(newnode->refupperindexpr, sbsref->refupperindexpr,
					   List *);
				MUTATE(newnode->reflowerindexpr, sbsref->reflowerindexpr,
					   List *);
				MUTATE(newnode->refexpr, sbsref->refexpr,
					   Expr *);
				MUTATE(newnode->refassgnexpr, sbsref->refassgnexpr,
					   Expr *);

				return (Node *) newnode;
			}
			break;
		case T_FuncExpr:
			{
				FuncExpr   *expr = (FuncExpr *) node;
				FuncExpr   *newnode;

				FLATCOPY(newnode, expr, FuncExpr);
				MUTATE(newnode->args, expr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_NamedArgExpr:
			{
				NamedArgExpr *nexpr = (NamedArgExpr *) node;
				NamedArgExpr *newnode;

				FLATCOPY(newnode, nexpr, NamedArgExpr);
				MUTATE(newnode->arg, nexpr->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_OpExpr:
			{
				OpExpr	   *expr = (OpExpr *) node;
				OpExpr	   *newnode;

				FLATCOPY(newnode, expr, OpExpr);
				MUTATE(newnode->args, expr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_DistinctExpr:
			{
				DistinctExpr *expr = (DistinctExpr *) node;
				DistinctExpr *newnode;

				FLATCOPY(newnode, expr, DistinctExpr);
				MUTATE(newnode->args, expr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_NullIfExpr:
			{
				NullIfExpr *expr = (NullIfExpr *) node;
				NullIfExpr *newnode;

				FLATCOPY(newnode, expr, NullIfExpr);
				MUTATE(newnode->args, expr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;
				ScalarArrayOpExpr *newnode;

				FLATCOPY(newnode, expr, ScalarArrayOpExpr);
				MUTATE(newnode->args, expr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;
				BoolExpr   *newnode;

				FLATCOPY(newnode, expr, BoolExpr);
				MUTATE(newnode->args, expr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;
				SubLink    *newnode;

				FLATCOPY(newnode, sublink, SubLink);
				MUTATE(newnode->testexpr, sublink->testexpr, Node *);

				/*
				 * Also invoke the mutator on the sublink's Query node, so it
				 * can recurse into the sub-query if it wants to.
				 */
				MUTATE(newnode->subselect, sublink->subselect, Node *);
				return (Node *) newnode;
			}
			break;
		case T_SubPlan:
			{
				SubPlan    *subplan = (SubPlan *) node;
				SubPlan    *newnode;

				FLATCOPY(newnode, subplan, SubPlan);
				/* transform testexpr */
				MUTATE(newnode->testexpr, subplan->testexpr, Node *);
				/* transform args list (params to be passed to subplan) */
				MUTATE(newnode->args, subplan->args, List *);
				/* but not the sub-Plan itself, which is referenced as-is */
				return (Node *) newnode;
			}
			break;
		case T_AlternativeSubPlan:
			{
				AlternativeSubPlan *asplan = (AlternativeSubPlan *) node;
				AlternativeSubPlan *newnode;

				FLATCOPY(newnode, asplan, AlternativeSubPlan);
				MUTATE(newnode->subplans, asplan->subplans, List *);
				return (Node *) newnode;
			}
			break;
		case T_FieldSelect:
			{
				FieldSelect *fselect = (FieldSelect *) node;
				FieldSelect *newnode;

				FLATCOPY(newnode, fselect, FieldSelect);
				MUTATE(newnode->arg, fselect->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_FieldStore:
			{
				FieldStore *fstore = (FieldStore *) node;
				FieldStore *newnode;

				FLATCOPY(newnode, fstore, FieldStore);
				MUTATE(newnode->arg, fstore->arg, Expr *);
				MUTATE(newnode->newvals, fstore->newvals, List *);
				newnode->fieldnums = list_copy(fstore->fieldnums);
				return (Node *) newnode;
			}
			break;
		case T_RelabelType:
			{
				RelabelType *relabel = (RelabelType *) node;
				RelabelType *newnode;

				FLATCOPY(newnode, relabel, RelabelType);
				MUTATE(newnode->arg, relabel->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_CoerceViaIO:
			{
				CoerceViaIO *iocoerce = (CoerceViaIO *) node;
				CoerceViaIO *newnode;

				FLATCOPY(newnode, iocoerce, CoerceViaIO);
				MUTATE(newnode->arg, iocoerce->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_ArrayCoerceExpr:
			{
				ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) node;
				ArrayCoerceExpr *newnode;

				FLATCOPY(newnode, acoerce, ArrayCoerceExpr);
				MUTATE(newnode->arg, acoerce->arg, Expr *);
				MUTATE(newnode->elemexpr, acoerce->elemexpr, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_ConvertRowtypeExpr:
			{
				ConvertRowtypeExpr *convexpr = (ConvertRowtypeExpr *) node;
				ConvertRowtypeExpr *newnode;

				FLATCOPY(newnode, convexpr, ConvertRowtypeExpr);
				MUTATE(newnode->arg, convexpr->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_CollateExpr:
			{
				CollateExpr *collate = (CollateExpr *) node;
				CollateExpr *newnode;

				FLATCOPY(newnode, collate, CollateExpr);
				MUTATE(newnode->arg, collate->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;
				CaseExpr   *newnode;

				FLATCOPY(newnode, caseexpr, CaseExpr);
				MUTATE(newnode->arg, caseexpr->arg, Expr *);
				MUTATE(newnode->args, caseexpr->args, List *);
				MUTATE(newnode->defresult, caseexpr->defresult, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_CaseWhen:
			{
				CaseWhen   *casewhen = (CaseWhen *) node;
				CaseWhen   *newnode;

				FLATCOPY(newnode, casewhen, CaseWhen);
				MUTATE(newnode->expr, casewhen->expr, Expr *);
				MUTATE(newnode->result, casewhen->result, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_ArrayExpr:
			{
				ArrayExpr  *arrayexpr = (ArrayExpr *) node;
				ArrayExpr  *newnode;

				FLATCOPY(newnode, arrayexpr, ArrayExpr);
				MUTATE(newnode->elements, arrayexpr->elements, List *);
				return (Node *) newnode;
			}
			break;
		case T_RowExpr:
			{
				RowExpr    *rowexpr = (RowExpr *) node;
				RowExpr    *newnode;

				FLATCOPY(newnode, rowexpr, RowExpr);
				MUTATE(newnode->args, rowexpr->args, List *);
				/* Assume colnames needn't be duplicated */
				return (Node *) newnode;
			}
			break;
		case T_RowCompareExpr:
			{
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;
				RowCompareExpr *newnode;

				FLATCOPY(newnode, rcexpr, RowCompareExpr);
				MUTATE(newnode->largs, rcexpr->largs, List *);
				MUTATE(newnode->rargs, rcexpr->rargs, List *);
				return (Node *) newnode;
			}
			break;
		case T_CoalesceExpr:
			{
				CoalesceExpr *coalesceexpr = (CoalesceExpr *) node;
				CoalesceExpr *newnode;

				FLATCOPY(newnode, coalesceexpr, CoalesceExpr);
				MUTATE(newnode->args, coalesceexpr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_MinMaxExpr:
			{
				MinMaxExpr *minmaxexpr = (MinMaxExpr *) node;
				MinMaxExpr *newnode;

				FLATCOPY(newnode, minmaxexpr, MinMaxExpr);
				MUTATE(newnode->args, minmaxexpr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_XmlExpr:
			{
				XmlExpr    *xexpr = (XmlExpr *) node;
				XmlExpr    *newnode;

				FLATCOPY(newnode, xexpr, XmlExpr);
				MUTATE(newnode->named_args, xexpr->named_args, List *);
				/* assume mutator does not care about arg_names */
				MUTATE(newnode->args, xexpr->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_JsonReturning:
			{
				JsonReturning *jr = (JsonReturning *) node;
				JsonReturning *newnode;

				FLATCOPY(newnode, jr, JsonReturning);
				MUTATE(newnode->format, jr->format, JsonFormat *);

				return (Node *) newnode;
			}
		case T_JsonValueExpr:
			{
				JsonValueExpr *jve = (JsonValueExpr *) node;
				JsonValueExpr *newnode;

				FLATCOPY(newnode, jve, JsonValueExpr);
				MUTATE(newnode->raw_expr, jve->raw_expr, Expr *);
				MUTATE(newnode->formatted_expr, jve->formatted_expr, Expr *);
				MUTATE(newnode->format, jve->format, JsonFormat *);

				return (Node *) newnode;
			}
		case T_JsonConstructorExpr:
			{
				JsonConstructorExpr *jce = (JsonConstructorExpr *) node;
				JsonConstructorExpr *newnode;

				FLATCOPY(newnode, jce, JsonConstructorExpr);
				MUTATE(newnode->args, jce->args, List *);
				MUTATE(newnode->func, jce->func, Expr *);
				MUTATE(newnode->coercion, jce->coercion, Expr *);
				MUTATE(newnode->returning, jce->returning, JsonReturning *);

				return (Node *) newnode;
			}
		case T_JsonIsPredicate:
			{
				JsonIsPredicate *pred = (JsonIsPredicate *) node;
				JsonIsPredicate *newnode;

				FLATCOPY(newnode, pred, JsonIsPredicate);
				MUTATE(newnode->expr, pred->expr, Node *);
				MUTATE(newnode->format, pred->format, JsonFormat *);

				return (Node *) newnode;
			}
		case T_JsonExpr:
			{
				JsonExpr   *jexpr = (JsonExpr *) node;
				JsonExpr   *newnode;

				FLATCOPY(newnode, jexpr, JsonExpr);
				MUTATE(newnode->formatted_expr, jexpr->formatted_expr, Node *);
				MUTATE(newnode->path_spec, jexpr->path_spec, Node *);
				MUTATE(newnode->passing_values, jexpr->passing_values, List *);
				/* assume mutator does not care about passing_names */
				MUTATE(newnode->on_empty, jexpr->on_empty, JsonBehavior *);
				MUTATE(newnode->on_error, jexpr->on_error, JsonBehavior *);
				return (Node *) newnode;
			}
			break;
		case T_JsonBehavior:
			{
				JsonBehavior *behavior = (JsonBehavior *) node;
				JsonBehavior *newnode;

				FLATCOPY(newnode, behavior, JsonBehavior);
				MUTATE(newnode->expr, behavior->expr, Node *);
				return (Node *) newnode;
			}
			break;
		case T_NullTest:
			{
				NullTest   *ntest = (NullTest *) node;
				NullTest   *newnode;

				FLATCOPY(newnode, ntest, NullTest);
				MUTATE(newnode->arg, ntest->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_BooleanTest:
			{
				BooleanTest *btest = (BooleanTest *) node;
				BooleanTest *newnode;

				FLATCOPY(newnode, btest, BooleanTest);
				MUTATE(newnode->arg, btest->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_CoerceToDomain:
			{
				CoerceToDomain *ctest = (CoerceToDomain *) node;
				CoerceToDomain *newnode;

				FLATCOPY(newnode, ctest, CoerceToDomain);
				MUTATE(newnode->arg, ctest->arg, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_TargetEntry:
			{
				TargetEntry *targetentry = (TargetEntry *) node;
				TargetEntry *newnode;

				FLATCOPY(newnode, targetentry, TargetEntry);
				MUTATE(newnode->expr, targetentry->expr, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_Query:
			/* Do nothing with a sub-Query, per discussion above */
			return node;
		case T_WindowClause:
			{
				WindowClause *wc = (WindowClause *) node;
				WindowClause *newnode;

				FLATCOPY(newnode, wc, WindowClause);
				MUTATE(newnode->partitionClause, wc->partitionClause, List *);
				MUTATE(newnode->orderClause, wc->orderClause, List *);
				MUTATE(newnode->startOffset, wc->startOffset, Node *);
				MUTATE(newnode->endOffset, wc->endOffset, Node *);
				return (Node *) newnode;
			}
			break;
		case T_CTECycleClause:
			{
				CTECycleClause *cc = (CTECycleClause *) node;
				CTECycleClause *newnode;

				FLATCOPY(newnode, cc, CTECycleClause);
				MUTATE(newnode->cycle_mark_value, cc->cycle_mark_value, Node *);
				MUTATE(newnode->cycle_mark_default, cc->cycle_mark_default, Node *);
				return (Node *) newnode;
			}
			break;
		case T_CommonTableExpr:
			{
				CommonTableExpr *cte = (CommonTableExpr *) node;
				CommonTableExpr *newnode;

				FLATCOPY(newnode, cte, CommonTableExpr);

				/*
				 * Also invoke the mutator on the CTE's Query node, so it can
				 * recurse into the sub-query if it wants to.
				 */
				MUTATE(newnode->ctequery, cte->ctequery, Node *);

				MUTATE(newnode->search_clause, cte->search_clause, CTESearchClause *);
				MUTATE(newnode->cycle_clause, cte->cycle_clause, CTECycleClause *);

				return (Node *) newnode;
			}
			break;
		case T_PartitionBoundSpec:
			{
				PartitionBoundSpec *pbs = (PartitionBoundSpec *) node;
				PartitionBoundSpec *newnode;

				FLATCOPY(newnode, pbs, PartitionBoundSpec);
				MUTATE(newnode->listdatums, pbs->listdatums, List *);
				MUTATE(newnode->lowerdatums, pbs->lowerdatums, List *);
				MUTATE(newnode->upperdatums, pbs->upperdatums, List *);
				return (Node *) newnode;
			}
			break;
		case T_PartitionRangeDatum:
			{
				PartitionRangeDatum *prd = (PartitionRangeDatum *) node;
				PartitionRangeDatum *newnode;

				FLATCOPY(newnode, prd, PartitionRangeDatum);
				MUTATE(newnode->value, prd->value, Node *);
				return (Node *) newnode;
			}
			break;
		case T_List:
			{
				/*
				 * We assume the mutator isn't interested in the list nodes
				 * per se, so just invoke it on each list element. NOTE: this
				 * would fail badly on a list with integer elements!
				 */
				List	   *resultlist;
				ListCell   *temp;

				resultlist = NIL;
				foreach(temp, (List *) node)
				{
					resultlist = lappend(resultlist,
										 mutator((Node *) lfirst(temp),
												 context));
				}
				return (Node *) resultlist;
			}
			break;
		case T_FromExpr:
			{
				FromExpr   *from = (FromExpr *) node;
				FromExpr   *newnode;

				FLATCOPY(newnode, from, FromExpr);
				MUTATE(newnode->fromlist, from->fromlist, List *);
				MUTATE(newnode->quals, from->quals, Node *);
				return (Node *) newnode;
			}
			break;
		case T_OnConflictExpr:
			{
				OnConflictExpr *oc = (OnConflictExpr *) node;
				OnConflictExpr *newnode;

				FLATCOPY(newnode, oc, OnConflictExpr);
				MUTATE(newnode->arbiterElems, oc->arbiterElems, List *);
				MUTATE(newnode->arbiterWhere, oc->arbiterWhere, Node *);
				MUTATE(newnode->onConflictSet, oc->onConflictSet, List *);
				MUTATE(newnode->onConflictWhere, oc->onConflictWhere, Node *);
				MUTATE(newnode->exclRelTlist, oc->exclRelTlist, List *);

				return (Node *) newnode;
			}
			break;
		case T_MergeAction:
			{
				MergeAction *action = (MergeAction *) node;
				MergeAction *newnode;

				FLATCOPY(newnode, action, MergeAction);
				MUTATE(newnode->qual, action->qual, Node *);
				MUTATE(newnode->targetList, action->targetList, List *);

				return (Node *) newnode;
			}
			break;
		case T_PartitionPruneStepOp:
			{
				PartitionPruneStepOp *opstep = (PartitionPruneStepOp *) node;
				PartitionPruneStepOp *newnode;

				FLATCOPY(newnode, opstep, PartitionPruneStepOp);
				MUTATE(newnode->exprs, opstep->exprs, List *);

				return (Node *) newnode;
			}
			break;
		case T_PartitionPruneStepCombine:
			/* no expression sub-nodes */
			return copyObject(node);
		case T_JoinExpr:
			{
				JoinExpr   *join = (JoinExpr *) node;
				JoinExpr   *newnode;

				FLATCOPY(newnode, join, JoinExpr);
				MUTATE(newnode->larg, join->larg, Node *);
				MUTATE(newnode->rarg, join->rarg, Node *);
				MUTATE(newnode->quals, join->quals, Node *);
				/* We do not mutate alias or using by default */
				return (Node *) newnode;
			}
			break;
		case T_SetOperationStmt:
			{
				SetOperationStmt *setop = (SetOperationStmt *) node;
				SetOperationStmt *newnode;

				FLATCOPY(newnode, setop, SetOperationStmt);
				MUTATE(newnode->larg, setop->larg, Node *);
				MUTATE(newnode->rarg, setop->rarg, Node *);
				/* We do not mutate groupClauses by default */
				return (Node *) newnode;
			}
			break;
		case T_IndexClause:
			{
				IndexClause *iclause = (IndexClause *) node;
				IndexClause *newnode;

				FLATCOPY(newnode, iclause, IndexClause);
				MUTATE(newnode->rinfo, iclause->rinfo, RestrictInfo *);
				MUTATE(newnode->indexquals, iclause->indexquals, List *);
				return (Node *) newnode;
			}
			break;
		case T_PlaceHolderVar:
			{
				PlaceHolderVar *phv = (PlaceHolderVar *) node;
				PlaceHolderVar *newnode;

				FLATCOPY(newnode, phv, PlaceHolderVar);
				MUTATE(newnode->phexpr, phv->phexpr, Expr *);
				/* Assume we need not copy the relids bitmapsets */
				return (Node *) newnode;
			}
			break;
		case T_InferenceElem:
			{
				InferenceElem *inferenceelemdexpr = (InferenceElem *) node;
				InferenceElem *newnode;

				FLATCOPY(newnode, inferenceelemdexpr, InferenceElem);
				MUTATE(newnode->expr, newnode->expr, Node *);
				return (Node *) newnode;
			}
			break;
		case T_AppendRelInfo:
			{
				AppendRelInfo *appinfo = (AppendRelInfo *) node;
				AppendRelInfo *newnode;

				FLATCOPY(newnode, appinfo, AppendRelInfo);
				MUTATE(newnode->translated_vars, appinfo->translated_vars, List *);
				/* Assume nothing need be done with parent_colnos[] */
				return (Node *) newnode;
			}
			break;
		case T_PlaceHolderInfo:
			{
				PlaceHolderInfo *phinfo = (PlaceHolderInfo *) node;
				PlaceHolderInfo *newnode;

				FLATCOPY(newnode, phinfo, PlaceHolderInfo);
				MUTATE(newnode->ph_var, phinfo->ph_var, PlaceHolderVar *);
				/* Assume we need not copy the relids bitmapsets */
				return (Node *) newnode;
			}
			break;
		case T_RangeTblFunction:
			{
				RangeTblFunction *rtfunc = (RangeTblFunction *) node;
				RangeTblFunction *newnode;

				FLATCOPY(newnode, rtfunc, RangeTblFunction);
				MUTATE(newnode->funcexpr, rtfunc->funcexpr, Node *);
				/* Assume we need not copy the coldef info lists */
				return (Node *) newnode;
			}
			break;
		case T_TableSampleClause:
			{
				TableSampleClause *tsc = (TableSampleClause *) node;
				TableSampleClause *newnode;

				FLATCOPY(newnode, tsc, TableSampleClause);
				MUTATE(newnode->args, tsc->args, List *);
				MUTATE(newnode->repeatable, tsc->repeatable, Expr *);
				return (Node *) newnode;
			}
			break;
		case T_TableFunc:
			{
				TableFunc  *tf = (TableFunc *) node;
				TableFunc  *newnode;

				FLATCOPY(newnode, tf, TableFunc);
				MUTATE(newnode->ns_uris, tf->ns_uris, List *);
				MUTATE(newnode->docexpr, tf->docexpr, Node *);
				MUTATE(newnode->rowexpr, tf->rowexpr, Node *);
				MUTATE(newnode->colexprs, tf->colexprs, List *);
				MUTATE(newnode->coldefexprs, tf->coldefexprs, List *);
				MUTATE(newnode->colvalexprs, tf->colvalexprs, List *);
				MUTATE(newnode->passingvalexprs, tf->passingvalexprs, List *);
				return (Node *) newnode;
			}
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(node));
			break;
	}
	/* can't get here, but keep compiler happy */
	return NULL;
}


/*
 * query_tree_mutator --- initiate modification of a Query's expressions
 *
 * This routine exists just to reduce the number of places that need to know
 * where all the expression subtrees of a Query are.  Note it can be used
 * for starting a walk at top level of a Query regardless of whether the
 * mutator intends to descend into subqueries.  It is also useful for
 * descending into subqueries within a mutator.
 *
 * Some callers want to suppress mutating of certain items in the Query,
 * typically because they need to process them specially, or don't actually
 * want to recurse into subqueries.  This is supported by the flags argument,
 * which is the bitwise OR of flag values to suppress mutating of
 * indicated items.  (More flag bits may be added as needed.)
 *
 * Normally the top-level Query node itself is copied, but some callers want
 * it to be modified in-place; they must pass QTW_DONT_COPY_QUERY in flags.
 * All modified substructure is safely copied in any case.
 */
Query *
query_tree_mutator_impl(Query *query,
						tree_mutator_callback mutator,
						void *context,
						int flags)
{
	Assert(query != NULL && IsA(query, Query));

	if (!(flags & QTW_DONT_COPY_QUERY))
	{
		Query	   *newquery;

		FLATCOPY(newquery, query, Query);
		query = newquery;
	}

	MUTATE(query->targetList, query->targetList, List *);
	MUTATE(query->withCheckOptions, query->withCheckOptions, List *);
	MUTATE(query->onConflict, query->onConflict, OnConflictExpr *);
	MUTATE(query->mergeActionList, query->mergeActionList, List *);
	MUTATE(query->mergeJoinCondition, query->mergeJoinCondition, Node *);
	MUTATE(query->returningList, query->returningList, List *);
	MUTATE(query->jointree, query->jointree, FromExpr *);
	MUTATE(query->setOperations, query->setOperations, Node *);
	MUTATE(query->havingQual, query->havingQual, Node *);
	MUTATE(query->limitOffset, query->limitOffset, Node *);
	MUTATE(query->limitCount, query->limitCount, Node *);

	/*
	 * Most callers aren't interested in SortGroupClause nodes since those
	 * don't contain actual expressions. However they do contain OIDs, which
	 * may be of interest to some mutators.
	 */

	if ((flags & QTW_EXAMINE_SORTGROUP))
	{
		MUTATE(query->groupClause, query->groupClause, List *);
		MUTATE(query->windowClause, query->windowClause, List *);
		MUTATE(query->sortClause, query->sortClause, List *);
		MUTATE(query->distinctClause, query->distinctClause, List *);
	}
	else
	{
		/*
		 * But we need to mutate the expressions under WindowClause nodes even
		 * if we're not interested in SortGroupClause nodes.
		 */
		List	   *resultlist;
		ListCell   *temp;

		resultlist = NIL;
		foreach(temp, query->windowClause)
		{
			WindowClause *wc = lfirst_node(WindowClause, temp);
			WindowClause *newnode;

			FLATCOPY(newnode, wc, WindowClause);
			MUTATE(newnode->startOffset, wc->startOffset, Node *);
			MUTATE(newnode->endOffset, wc->endOffset, Node *);

			resultlist = lappend(resultlist, (Node *) newnode);
		}
		query->windowClause = resultlist;
	}

	/*
	 * groupingSets and rowMarks are not mutated:
	 *
	 * groupingSets contain only ressortgroup refs (integers) which are
	 * meaningless without the groupClause or tlist. Accordingly, any mutator
	 * that needs to care about them needs to handle them itself in its Query
	 * processing.
	 *
	 * rowMarks contains only rangetable indexes (and flags etc.) and
	 * therefore should be handled at Query level similarly.
	 */

	if (!(flags & QTW_IGNORE_CTE_SUBQUERIES))
		MUTATE(query->cteList, query->cteList, List *);
	else						/* else copy CTE list as-is */
		query->cteList = copyObject(query->cteList);
	query->rtable = range_table_mutator(query->rtable,
										mutator, context, flags);
	return query;
}

/*
 * range_table_mutator is just the part of query_tree_mutator that processes
 * a query's rangetable.  This is split out since it can be useful on
 * its own.
 */
List *
range_table_mutator_impl(List *rtable,
						 tree_mutator_callback mutator,
						 void *context,
						 int flags)
{
	List	   *newrt = NIL;
	ListCell   *rt;

	foreach(rt, rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);
		RangeTblEntry *newrte;

		FLATCOPY(newrte, rte, RangeTblEntry);
		switch (rte->rtekind)
		{
			case RTE_RELATION:
				MUTATE(newrte->tablesample, rte->tablesample,
					   TableSampleClause *);
				/* we don't bother to copy eref, aliases, etc; OK? */
				break;
			case RTE_SUBQUERY:
				if (!(flags & QTW_IGNORE_RT_SUBQUERIES))
					MUTATE(newrte->subquery, rte->subquery, Query *);
				else
				{
					/* else, copy RT subqueries as-is */
					newrte->subquery = copyObject(rte->subquery);
				}
				break;
			case RTE_JOIN:
				if (!(flags & QTW_IGNORE_JOINALIASES))
					MUTATE(newrte->joinaliasvars, rte->joinaliasvars, List *);
				else
				{
					/* else, copy join aliases as-is */
					newrte->joinaliasvars = copyObject(rte->joinaliasvars);
				}
				break;
			case RTE_FUNCTION:
				MUTATE(newrte->functions, rte->functions, List *);
				break;
			case RTE_TABLEFUNC:
				MUTATE(newrte->tablefunc, rte->tablefunc, TableFunc *);
				break;
			case RTE_VALUES:
				MUTATE(newrte->values_lists, rte->values_lists, List *);
				break;
			case RTE_CTE:
			case RTE_NAMEDTUPLESTORE:
			case RTE_RESULT:
				/* nothing to do */
				break;
			case RTE_GROUP:
				if (!(flags & QTW_IGNORE_GROUPEXPRS))
					MUTATE(newrte->groupexprs, rte->groupexprs, List *);
				else
				{
					/* else, copy grouping exprs as-is */
					newrte->groupexprs = copyObject(rte->groupexprs);
				}
				break;
		}
		MUTATE(newrte->securityQuals, rte->securityQuals, List *);
		newrt = lappend(newrt, newrte);
	}
	return newrt;
}

/*
 * query_or_expression_tree_walker --- hybrid form
 *
 * This routine will invoke query_tree_walker if called on a Query node,
 * else will invoke the walker directly.  This is a useful way of starting
 * the recursion when the walker's normal change of state is not appropriate
 * for the outermost Query node.
 */
bool
query_or_expression_tree_walker_impl(Node *node,
									 tree_walker_callback walker,
									 void *context,
									 int flags)
{
	if (node && IsA(node, Query))
		return query_tree_walker((Query *) node,
								 walker,
								 context,
								 flags);
	else
		return WALK(node);
}

/*
 * query_or_expression_tree_mutator --- hybrid form
 *
 * This routine will invoke query_tree_mutator if called on a Query node,
 * else will invoke the mutator directly.  This is a useful way of starting
 * the recursion when the mutator's normal change of state is not appropriate
 * for the outermost Query node.
 */
Node *
query_or_expression_tree_mutator_impl(Node *node,
									  tree_mutator_callback mutator,
									  void *context,
									  int flags)
{
	if (node && IsA(node, Query))
		return (Node *) query_tree_mutator((Query *) node,
										   mutator,
										   context,
										   flags);
	else
		return mutator(node, context);
}


/*
 * raw_expression_tree_walker --- walk raw parse trees
 *
 * This has exactly the same API as expression_tree_walker, but instead of
 * walking post-analysis parse trees, it knows how to walk the node types
 * found in raw grammar output.  (There is not currently any need for a
 * combined walker, so we keep them separate in the name of efficiency.)
 * Unlike expression_tree_walker, there is no special rule about query
 * boundaries: we descend to everything that's possibly interesting.
 *
 * Currently, the node type coverage here extends only to DML statements
 * (SELECT/INSERT/UPDATE/DELETE/MERGE) and nodes that can appear in them,
 * because this is used mainly during analysis of CTEs, and only DML
 * statements can appear in CTEs.
 */
bool
raw_expression_tree_walker_impl(Node *node,
								tree_walker_callback walker,
								void *context)
{
	ListCell   *temp;

	/*
	 * The walker has already visited the current node, and so we need only
	 * recurse into any sub-nodes it has.
	 */
	if (node == NULL)
		return false;

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	switch (nodeTag(node))
	{
		case T_JsonFormat:
		case T_SetToDefault:
		case T_CurrentOfExpr:
		case T_SQLValueFunction:
		case T_Integer:
		case T_Float:
		case T_Boolean:
		case T_String:
		case T_BitString:
		case T_ParamRef:
		case T_A_Const:
		case T_A_Star:
		case T_MergeSupportFunc:
			/* primitive node types with no subnodes */
			break;
		case T_Alias:
			/* we assume the colnames list isn't interesting */
			break;
		case T_RangeVar:
			return WALK(((RangeVar *) node)->alias);
		case T_GroupingFunc:
			return WALK(((GroupingFunc *) node)->args);
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;

				if (WALK(sublink->testexpr))
					return true;
				/* we assume the operName is not interesting */
				if (WALK(sublink->subselect))
					return true;
			}
			break;
		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;

				if (WALK(caseexpr->arg))
					return true;
				/* we assume walker doesn't care about CaseWhens, either */
				foreach(temp, caseexpr->args)
				{
					CaseWhen   *when = lfirst_node(CaseWhen, temp);

					if (WALK(when->expr))
						return true;
					if (WALK(when->result))
						return true;
				}
				if (WALK(caseexpr->defresult))
					return true;
			}
			break;
		case T_RowExpr:
			/* Assume colnames isn't interesting */
			return WALK(((RowExpr *) node)->args);
		case T_CoalesceExpr:
			return WALK(((CoalesceExpr *) node)->args);
		case T_MinMaxExpr:
			return WALK(((MinMaxExpr *) node)->args);
		case T_XmlExpr:
			{
				XmlExpr    *xexpr = (XmlExpr *) node;

				if (WALK(xexpr->named_args))
					return true;
				/* we assume walker doesn't care about arg_names */
				if (WALK(xexpr->args))
					return true;
			}
			break;
		case T_JsonReturning:
			return WALK(((JsonReturning *) node)->format);
		case T_JsonValueExpr:
			{
				JsonValueExpr *jve = (JsonValueExpr *) node;

				if (WALK(jve->raw_expr))
					return true;
				if (WALK(jve->formatted_expr))
					return true;
				if (WALK(jve->format))
					return true;
			}
			break;
		case T_JsonParseExpr:
			{
				JsonParseExpr *jpe = (JsonParseExpr *) node;

				if (WALK(jpe->expr))
					return true;
				if (WALK(jpe->output))
					return true;
			}
			break;
		case T_JsonScalarExpr:
			{
				JsonScalarExpr *jse = (JsonScalarExpr *) node;

				if (WALK(jse->expr))
					return true;
				if (WALK(jse->output))
					return true;
			}
			break;
		case T_JsonSerializeExpr:
			{
				JsonSerializeExpr *jse = (JsonSerializeExpr *) node;

				if (WALK(jse->expr))
					return true;
				if (WALK(jse->output))
					return true;
			}
			break;
		case T_JsonConstructorExpr:
			{
				JsonConstructorExpr *ctor = (JsonConstructorExpr *) node;

				if (WALK(ctor->args))
					return true;
				if (WALK(ctor->func))
					return true;
				if (WALK(ctor->coercion))
					return true;
				if (WALK(ctor->returning))
					return true;
			}
			break;
		case T_JsonIsPredicate:
			return WALK(((JsonIsPredicate *) node)->expr);
		case T_JsonArgument:
			return WALK(((JsonArgument *) node)->val);
		case T_JsonFuncExpr:
			{
				JsonFuncExpr *jfe = (JsonFuncExpr *) node;

				if (WALK(jfe->context_item))
					return true;
				if (WALK(jfe->pathspec))
					return true;
				if (WALK(jfe->passing))
					return true;
				if (WALK(jfe->output))
					return true;
				if (WALK(jfe->on_empty))
					return true;
				if (WALK(jfe->on_error))
					return true;
			}
			break;
		case T_JsonBehavior:
			{
				JsonBehavior *jb = (JsonBehavior *) node;

				if (WALK(jb->expr))
					return true;
			}
			break;
		case T_JsonTable:
			{
				JsonTable  *jt = (JsonTable *) node;

				if (WALK(jt->context_item))
					return true;
				if (WALK(jt->pathspec))
					return true;
				if (WALK(jt->passing))
					return true;
				if (WALK(jt->columns))
					return true;
				if (WALK(jt->on_error))
					return true;
			}
			break;
		case T_JsonTableColumn:
			{
				JsonTableColumn *jtc = (JsonTableColumn *) node;

				if (WALK(jtc->typeName))
					return true;
				if (WALK(jtc->on_empty))
					return true;
				if (WALK(jtc->on_error))
					return true;
				if (WALK(jtc->columns))
					return true;
			}
			break;
		case T_JsonTablePathSpec:
			return WALK(((JsonTablePathSpec *) node)->string);
		case T_NullTest:
			return WALK(((NullTest *) node)->arg);
		case T_BooleanTest:
			return WALK(((BooleanTest *) node)->arg);
		case T_JoinExpr:
			{
				JoinExpr   *join = (JoinExpr *) node;

				if (WALK(join->larg))
					return true;
				if (WALK(join->rarg))
					return true;
				if (WALK(join->quals))
					return true;
				if (WALK(join->alias))
					return true;
				/* using list is deemed uninteresting */
			}
			break;
		case T_IntoClause:
			{
				IntoClause *into = (IntoClause *) node;

				if (WALK(into->rel))
					return true;
				/* colNames, options are deemed uninteresting */
				/* viewQuery should be null in raw parsetree, but check it */
				if (WALK(into->viewQuery))
					return true;
			}
			break;
		case T_List:
			foreach(temp, (List *) node)
			{
				if (WALK((Node *) lfirst(temp)))
					return true;
			}
			break;
		case T_InsertStmt:
			{
				InsertStmt *stmt = (InsertStmt *) node;

				if (WALK(stmt->relation))
					return true;
				if (WALK(stmt->cols))
					return true;
				if (WALK(stmt->selectStmt))
					return true;
				if (WALK(stmt->onConflictClause))
					return true;
				if (WALK(stmt->returningList))
					return true;
				if (WALK(stmt->withClause))
					return true;
			}
			break;
		case T_DeleteStmt:
			{
				DeleteStmt *stmt = (DeleteStmt *) node;

				if (WALK(stmt->relation))
					return true;
				if (WALK(stmt->usingClause))
					return true;
				if (WALK(stmt->whereClause))
					return true;
				if (WALK(stmt->returningList))
					return true;
				if (WALK(stmt->withClause))
					return true;
			}
			break;
		case T_UpdateStmt:
			{
				UpdateStmt *stmt = (UpdateStmt *) node;

				if (WALK(stmt->relation))
					return true;
				if (WALK(stmt->targetList))
					return true;
				if (WALK(stmt->whereClause))
					return true;
				if (WALK(stmt->fromClause))
					return true;
				if (WALK(stmt->returningList))
					return true;
				if (WALK(stmt->withClause))
					return true;
			}
			break;
		case T_MergeStmt:
			{
				MergeStmt  *stmt = (MergeStmt *) node;

				if (WALK(stmt->relation))
					return true;
				if (WALK(stmt->sourceRelation))
					return true;
				if (WALK(stmt->joinCondition))
					return true;
				if (WALK(stmt->mergeWhenClauses))
					return true;
				if (WALK(stmt->returningList))
					return true;
				if (WALK(stmt->withClause))
					return true;
			}
			break;
		case T_MergeWhenClause:
			{
				MergeWhenClause *mergeWhenClause = (MergeWhenClause *) node;

				if (WALK(mergeWhenClause->condition))
					return true;
				if (WALK(mergeWhenClause->targetList))
					return true;
				if (WALK(mergeWhenClause->values))
					return true;
			}
			break;
		case T_SelectStmt:
			{
				SelectStmt *stmt = (SelectStmt *) node;

				if (WALK(stmt->distinctClause))
					return true;
				if (WALK(stmt->intoClause))
					return true;
				if (WALK(stmt->targetList))
					return true;
				if (WALK(stmt->fromClause))
					return true;
				if (WALK(stmt->whereClause))
					return true;
				if (WALK(stmt->groupClause))
					return true;
				if (WALK(stmt->havingClause))
					return true;
				if (WALK(stmt->windowClause))
					return true;
				if (WALK(stmt->valuesLists))
					return true;
				if (WALK(stmt->sortClause))
					return true;
				if (WALK(stmt->limitOffset))
					return true;
				if (WALK(stmt->limitCount))
					return true;
				if (WALK(stmt->lockingClause))
					return true;
				if (WALK(stmt->withClause))
					return true;
				if (WALK(stmt->larg))
					return true;
				if (WALK(stmt->rarg))
					return true;
			}
			break;
		case T_PLAssignStmt:
			{
				PLAssignStmt *stmt = (PLAssignStmt *) node;

				if (WALK(stmt->indirection))
					return true;
				if (WALK(stmt->val))
					return true;
			}
			break;
		case T_A_Expr:
			{
				A_Expr	   *expr = (A_Expr *) node;

				if (WALK(expr->lexpr))
					return true;
				if (WALK(expr->rexpr))
					return true;
				/* operator name is deemed uninteresting */
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;

				if (WALK(expr->args))
					return true;
			}
			break;
		case T_ColumnRef:
			/* we assume the fields contain nothing interesting */
			break;
		case T_FuncCall:
			{
				FuncCall   *fcall = (FuncCall *) node;

				if (WALK(fcall->args))
					return true;
				if (WALK(fcall->agg_order))
					return true;
				if (WALK(fcall->agg_filter))
					return true;
				if (WALK(fcall->over))
					return true;
				/* function name is deemed uninteresting */
			}
			break;
		case T_NamedArgExpr:
			return WALK(((NamedArgExpr *) node)->arg);
		case T_A_Indices:
			{
				A_Indices  *indices = (A_Indices *) node;

				if (WALK(indices->lidx))
					return true;
				if (WALK(indices->uidx))
					return true;
			}
			break;
		case T_A_Indirection:
			{
				A_Indirection *indir = (A_Indirection *) node;

				if (WALK(indir->arg))
					return true;
				if (WALK(indir->indirection))
					return true;
			}
			break;
		case T_A_ArrayExpr:
			return WALK(((A_ArrayExpr *) node)->elements);
		case T_ResTarget:
			{
				ResTarget  *rt = (ResTarget *) node;

				if (WALK(rt->indirection))
					return true;
				if (WALK(rt->val))
					return true;
			}
			break;
		case T_MultiAssignRef:
			return WALK(((MultiAssignRef *) node)->source);
		case T_TypeCast:
			{
				TypeCast   *tc = (TypeCast *) node;

				if (WALK(tc->arg))
					return true;
				if (WALK(tc->typeName))
					return true;
			}
			break;
		case T_CollateClause:
			return WALK(((CollateClause *) node)->arg);
		case T_SortBy:
			return WALK(((SortBy *) node)->node);
		case T_WindowDef:
			{
				WindowDef  *wd = (WindowDef *) node;

				if (WALK(wd->partitionClause))
					return true;
				if (WALK(wd->orderClause))
					return true;
				if (WALK(wd->startOffset))
					return true;
				if (WALK(wd->endOffset))
					return true;
			}
			break;
		case T_RangeSubselect:
			{
				RangeSubselect *rs = (RangeSubselect *) node;

				if (WALK(rs->subquery))
					return true;
				if (WALK(rs->alias))
					return true;
			}
			break;
		case T_RangeFunction:
			{
				RangeFunction *rf = (RangeFunction *) node;

				if (WALK(rf->functions))
					return true;
				if (WALK(rf->alias))
					return true;
				if (WALK(rf->coldeflist))
					return true;
			}
			break;
		case T_RangeTableSample:
			{
				RangeTableSample *rts = (RangeTableSample *) node;

				if (WALK(rts->relation))
					return true;
				/* method name is deemed uninteresting */
				if (WALK(rts->args))
					return true;
				if (WALK(rts->repeatable))
					return true;
			}
			break;
		case T_RangeTableFunc:
			{
				RangeTableFunc *rtf = (RangeTableFunc *) node;

				if (WALK(rtf->docexpr))
					return true;
				if (WALK(rtf->rowexpr))
					return true;
				if (WALK(rtf->namespaces))
					return true;
				if (WALK(rtf->columns))
					return true;
				if (WALK(rtf->alias))
					return true;
			}
			break;
		case T_RangeTableFuncCol:
			{
				RangeTableFuncCol *rtfc = (RangeTableFuncCol *) node;

				if (WALK(rtfc->colexpr))
					return true;
				if (WALK(rtfc->coldefexpr))
					return true;
			}
			break;
		case T_TypeName:
			{
				TypeName   *tn = (TypeName *) node;

				if (WALK(tn->typmods))
					return true;
				if (WALK(tn->arrayBounds))
					return true;
				/* type name itself is deemed uninteresting */
			}
			break;
		case T_ColumnDef:
			{
				ColumnDef  *coldef = (ColumnDef *) node;

				if (WALK(coldef->typeName))
					return true;
				if (WALK(coldef->raw_default))
					return true;
				if (WALK(coldef->collClause))
					return true;
				/* for now, constraints are ignored */
			}
			break;
		case T_IndexElem:
			{
				IndexElem  *indelem = (IndexElem *) node;

				if (WALK(indelem->expr))
					return true;
				/* collation and opclass names are deemed uninteresting */
			}
			break;
		case T_GroupingSet:
			return WALK(((GroupingSet *) node)->content);
		case T_LockingClause:
			return WALK(((LockingClause *) node)->lockedRels);
		case T_XmlSerialize:
			{
				XmlSerialize *xs = (XmlSerialize *) node;

				if (WALK(xs->expr))
					return true;
				if (WALK(xs->typeName))
					return true;
			}
			break;
		case T_WithClause:
			return WALK(((WithClause *) node)->ctes);
		case T_InferClause:
			{
				InferClause *stmt = (InferClause *) node;

				if (WALK(stmt->indexElems))
					return true;
				if (WALK(stmt->whereClause))
					return true;
			}
			break;
		case T_OnConflictClause:
			{
				OnConflictClause *stmt = (OnConflictClause *) node;

				if (WALK(stmt->infer))
					return true;
				if (WALK(stmt->targetList))
					return true;
				if (WALK(stmt->whereClause))
					return true;
			}
			break;
		case T_CommonTableExpr:
			/* search_clause and cycle_clause are not interesting here */
			return WALK(((CommonTableExpr *) node)->ctequery);
		case T_JsonOutput:
			{
				JsonOutput *out = (JsonOutput *) node;

				if (WALK(out->typeName))
					return true;
				if (WALK(out->returning))
					return true;
			}
			break;
		case T_JsonKeyValue:
			{
				JsonKeyValue *jkv = (JsonKeyValue *) node;

				if (WALK(jkv->key))
					return true;
				if (WALK(jkv->value))
					return true;
			}
			break;
		case T_JsonObjectConstructor:
			{
				JsonObjectConstructor *joc = (JsonObjectConstructor *) node;

				if (WALK(joc->output))
					return true;
				if (WALK(joc->exprs))
					return true;
			}
			break;
		case T_JsonArrayConstructor:
			{
				JsonArrayConstructor *jac = (JsonArrayConstructor *) node;

				if (WALK(jac->output))
					return true;
				if (WALK(jac->exprs))
					return true;
			}
			break;
		case T_JsonAggConstructor:
			{
				JsonAggConstructor *ctor = (JsonAggConstructor *) node;

				if (WALK(ctor->output))
					return true;
				if (WALK(ctor->agg_order))
					return true;
				if (WALK(ctor->agg_filter))
					return true;
				if (WALK(ctor->over))
					return true;
			}
			break;
		case T_JsonObjectAgg:
			{
				JsonObjectAgg *joa = (JsonObjectAgg *) node;

				if (WALK(joa->constructor))
					return true;
				if (WALK(joa->arg))
					return true;
			}
			break;
		case T_JsonArrayAgg:
			{
				JsonArrayAgg *jaa = (JsonArrayAgg *) node;

				if (WALK(jaa->constructor))
					return true;
				if (WALK(jaa->arg))
					return true;
			}
			break;
		case T_JsonArrayQueryConstructor:
			{
				JsonArrayQueryConstructor *jaqc = (JsonArrayQueryConstructor *) node;

				if (WALK(jaqc->output))
					return true;
				if (WALK(jaqc->query))
					return true;
			}
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(node));
			break;
	}
	return false;
}

/*
 * planstate_tree_walker --- walk plan state trees
 *
 * The walker has already visited the current node, and so we need only
 * recurse into any sub-nodes it has.
 */
bool
planstate_tree_walker_impl(PlanState *planstate,
						   planstate_tree_walker_callback walker,
						   void *context)
{
	Plan	   *plan = planstate->plan;
	ListCell   *lc;

	/* We don't need implicit coercions to Node here */
#define PSWALK(n) walker(n, context)

	/* Guard against stack overflow due to overly complex plan trees */
	check_stack_depth();

	/* initPlan-s */
	if (planstate_walk_subplans(planstate->initPlan, walker, context))
		return true;

	/* lefttree */
	if (outerPlanState(planstate))
	{
		if (PSWALK(outerPlanState(planstate)))
			return true;
	}

	/* righttree */
	if (innerPlanState(planstate))
	{
		if (PSWALK(innerPlanState(planstate)))
			return true;
	}

	/* special child plans */
	switch (nodeTag(plan))
	{
		case T_Append:
			if (planstate_walk_members(((AppendState *) planstate)->appendplans,
									   ((AppendState *) planstate)->as_nplans,
									   walker, context))
				return true;
			break;
		case T_MergeAppend:
			if (planstate_walk_members(((MergeAppendState *) planstate)->mergeplans,
									   ((MergeAppendState *) planstate)->ms_nplans,
									   walker, context))
				return true;
			break;
		case T_BitmapAnd:
			if (planstate_walk_members(((BitmapAndState *) planstate)->bitmapplans,
									   ((BitmapAndState *) planstate)->nplans,
									   walker, context))
				return true;
			break;
		case T_BitmapOr:
			if (planstate_walk_members(((BitmapOrState *) planstate)->bitmapplans,
									   ((BitmapOrState *) planstate)->nplans,
									   walker, context))
				return true;
			break;
		case T_SubqueryScan:
			if (PSWALK(((SubqueryScanState *) planstate)->subplan))
				return true;
			break;
		case T_CustomScan:
			foreach(lc, ((CustomScanState *) planstate)->custom_ps)
			{
				if (PSWALK(lfirst(lc)))
					return true;
			}
			break;
		default:
			break;
	}

	/* subPlan-s */
	if (planstate_walk_subplans(planstate->subPlan, walker, context))
		return true;

	return false;
}

/*
 * Walk a list of SubPlans (or initPlans, which also use SubPlan nodes).
 */
static bool
planstate_walk_subplans(List *plans,
						planstate_tree_walker_callback walker,
						void *context)
{
	ListCell   *lc;

	foreach(lc, plans)
	{
		SubPlanState *sps = lfirst_node(SubPlanState, lc);

		if (PSWALK(sps->planstate))
			return true;
	}

	return false;
}

/*
 * Walk the constituent plans of a ModifyTable, Append, MergeAppend,
 * BitmapAnd, or BitmapOr node.
 */
static bool
planstate_walk_members(PlanState **planstates, int nplans,
					   planstate_tree_walker_callback walker,
					   void *context)
{
	int			j;

	for (j = 0; j < nplans; j++)
	{
		if (PSWALK(planstates[j]))
			return true;
	}

	return false;
}
