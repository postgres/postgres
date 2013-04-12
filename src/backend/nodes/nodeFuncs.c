/*-------------------------------------------------------------------------
 *
 * nodeFuncs.c
 *		Various general-purpose manipulations of Node trees
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
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
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/relation.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


static bool expression_returns_set_walker(Node *node, void *context);
static int	leftmostLoc(int loc1, int loc2);


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
		case T_WindowFunc:
			type = ((const WindowFunc *) expr)->wintype;
			break;
		case T_ArrayRef:
			{
				const ArrayRef *arrayref = (const ArrayRef *) expr;

				/* slice and/or store operations yield the array type */
				if (arrayref->reflowerindexpr || arrayref->refassgnexpr)
					type = arrayref->refarraytype;
				else
					type = arrayref->refelemtype;
			}
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
					tent = (TargetEntry *) linitial(qtree->targetList);
					Assert(IsA(tent, TargetEntry));
					Assert(!tent->resjunk);
					type = exprType((Node *) tent->expr);
					if (sublink->subLinkType == ARRAY_SUBLINK)
					{
						type = get_array_type(type);
						if (!OidIsValid(type))
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_OBJECT),
									 errmsg("could not find array type for data type %s",
							format_type_be(exprType((Node *) tent->expr)))));
					}
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
						type = get_array_type(type);
						if (!OidIsValid(type))
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_OBJECT),
									 errmsg("could not find array type for data type %s",
									format_type_be(subplan->firstColType))));
					}
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
		case T_XmlExpr:
			if (((const XmlExpr *) expr)->op == IS_DOCUMENT)
				type = BOOLOID;
			else if (((const XmlExpr *) expr)->op == IS_XMLSERIALIZE)
				type = TEXTOID;
			else
				type = XMLOID;
			break;
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
 *	  if it can be determined.	In many cases, it can't and we return -1.
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
		case T_ArrayRef:
			/* typmod is the same for array or element */
			return ((const ArrayRef *) expr)->reftypmod;
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
					tent = (TargetEntry *) linitial(qtree->targetList);
					Assert(IsA(tent, TargetEntry));
					Assert(!tent->resjunk);
					return exprTypmod((Node *) tent->expr);
					/* note we don't need to care if it's an array */
				}
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
				else
				{
					/* for all other subplan types, result is boolean */
					return -1;
				}
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
					CaseWhen   *w = (CaseWhen *) lfirst(arg);

					Assert(IsA(w, CaseWhen));
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
				for_each_cell(arg, lnext(list_head(cexpr->args)))
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
				for_each_cell(arg, lnext(list_head(mexpr->args)))
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
 * relabel_to_typmod
 *		Add a RelabelType node that changes just the typmod of the expression.
 *
 * This is primarily intended to be used during planning.  Therefore, it
 * strips any existing RelabelType nodes to maintain the planner's invariant
 * that there are not adjacent RelabelTypes.
 */
Node *
relabel_to_typmod(Node *expr, int32 typmod)
{
	Oid			type = exprType(expr);
	Oid			coll = exprCollation(expr);

	/* Strip any existing RelabelType node(s) */
	while (expr && IsA(expr, RelabelType))
		expr = (Node *) ((RelabelType *) expr)->arg;

	/* Apply new typmod, preserving the previous exposed type and collation */
	return (Node *) makeRelabelType((Expr *) expr, type, typmod, coll,
									COERCE_EXPLICIT_CAST);
}

/*
 * expression_returns_set
 *	  Test whether an expression returns a set result.
 *
 * Because we use expression_tree_walker(), this can also be applied to
 * whole targetlists; it'll produce TRUE if any one of the tlist items
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

	/* Avoid recursion for some cases that can't return a set */
	if (IsA(node, Aggref))
		return false;
	if (IsA(node, WindowFunc))
		return false;
	if (IsA(node, DistinctExpr))
		return false;
	if (IsA(node, NullIfExpr))
		return false;
	if (IsA(node, ScalarArrayOpExpr))
		return false;
	if (IsA(node, BoolExpr))
		return false;
	if (IsA(node, SubLink))
		return false;
	if (IsA(node, SubPlan))
		return false;
	if (IsA(node, AlternativeSubPlan))
		return false;
	if (IsA(node, ArrayExpr))
		return false;
	if (IsA(node, RowExpr))
		return false;
	if (IsA(node, RowCompareExpr))
		return false;
	if (IsA(node, CoalesceExpr))
		return false;
	if (IsA(node, MinMaxExpr))
		return false;
	if (IsA(node, XmlExpr))
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
		case T_WindowFunc:
			coll = ((const WindowFunc *) expr)->wincollid;
			break;
		case T_ArrayRef:
			coll = ((const ArrayRef *) expr)->refcollid;
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
			coll = InvalidOid;	/* result is always boolean */
			break;
		case T_BoolExpr:
			coll = InvalidOid;	/* result is always boolean */
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
					tent = (TargetEntry *) linitial(qtree->targetList);
					Assert(IsA(tent, TargetEntry));
					Assert(!tent->resjunk);
					coll = exprCollation((Node *) tent->expr);
					/* collation doesn't change if it's converted to array */
				}
				else
				{
					/* for all other sublink types, result is boolean */
					coll = InvalidOid;
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
					/* for all other subplan types, result is boolean */
					coll = InvalidOid;
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
			coll = InvalidOid;	/* result is always composite */
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
			coll = InvalidOid;	/* result is always composite */
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
			coll = InvalidOid;	/* result is always composite */
			break;
		case T_RowCompareExpr:
			coll = InvalidOid;	/* result is always boolean */
			break;
		case T_CoalesceExpr:
			coll = ((const CoalesceExpr *) expr)->coalescecollid;
			break;
		case T_MinMaxExpr:
			coll = ((const MinMaxExpr *) expr)->minmaxcollid;
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
		case T_NullTest:
			coll = InvalidOid;	/* result is always boolean */
			break;
		case T_BooleanTest:
			coll = InvalidOid;	/* result is always boolean */
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
			coll = InvalidOid;	/* result is always boolean */
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
		case T_WindowFunc:
			((WindowFunc *) expr)->wincollid = collation;
			break;
		case T_ArrayRef:
			((ArrayRef *) expr)->refcollid = collation;
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
			Assert(!OidIsValid(collation));		/* result is always boolean */
			break;
		case T_BoolExpr:
			Assert(!OidIsValid(collation));		/* result is always boolean */
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
					tent = (TargetEntry *) linitial(qtree->targetList);
					Assert(IsA(tent, TargetEntry));
					Assert(!tent->resjunk);
					Assert(collation == exprCollation((Node *) tent->expr));
				}
				else
				{
					/* for all other sublink types, result is boolean */
					Assert(!OidIsValid(collation));
				}
			}
#endif   /* USE_ASSERT_CHECKING */
			break;
		case T_FieldSelect:
			((FieldSelect *) expr)->resultcollid = collation;
			break;
		case T_FieldStore:
			Assert(!OidIsValid(collation));		/* result is always composite */
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
			Assert(!OidIsValid(collation));		/* result is always composite */
			break;
		case T_CaseExpr:
			((CaseExpr *) expr)->casecollid = collation;
			break;
		case T_ArrayExpr:
			((ArrayExpr *) expr)->array_collid = collation;
			break;
		case T_RowExpr:
			Assert(!OidIsValid(collation));		/* result is always composite */
			break;
		case T_RowCompareExpr:
			Assert(!OidIsValid(collation));		/* result is always boolean */
			break;
		case T_CoalesceExpr:
			((CoalesceExpr *) expr)->coalescecollid = collation;
			break;
		case T_MinMaxExpr:
			((MinMaxExpr *) expr)->minmaxcollid = collation;
			break;
		case T_XmlExpr:
			Assert((((XmlExpr *) expr)->op == IS_XMLSERIALIZE) ?
				   (collation == DEFAULT_COLLATION_OID) :
				   (collation == InvalidOid));
			break;
		case T_NullTest:
			Assert(!OidIsValid(collation));		/* result is always boolean */
			break;
		case T_BooleanTest:
			Assert(!OidIsValid(collation));		/* result is always boolean */
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
			Assert(!OidIsValid(collation));		/* result is always boolean */
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
		case T_WindowFunc:
			/* function name should always be the first thing */
			loc = ((const WindowFunc *) expr)->location;
			break;
		case T_ArrayRef:
			/* just use array argument's location */
			loc = exprLocation((Node *) ((const ArrayRef *) expr)->refexpr);
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
		case T_XmlExpr:
			{
				const XmlExpr *xexpr = (const XmlExpr *) expr;

				/* consider both function name and leftmost arg */
				loc = leftmostLoc(xexpr->location,
								  exprLocation((Node *) xexpr->args));
			}
			break;
		case T_NullTest:
			/* just use argument's location */
			loc = exprLocation((Node *) ((const NullTest *) expr)->arg);
			break;
		case T_BooleanTest:
			/* just use argument's location */
			loc = exprLocation((Node *) ((const BooleanTest *) expr)->arg);
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
		case T_TypeName:
			loc = ((const TypeName *) expr)->location;
			break;
		case T_Constraint:
			loc = ((const Constraint *) expr)->location;
			break;
		case T_XmlSerialize:
			/* XMLSERIALIZE keyword should always be the first thing */
			loc = ((const XmlSerialize *) expr)->location;
			break;
		case T_WithClause:
			loc = ((const WithClause *) expr)->location;
			break;
		case T_CommonTableExpr:
			loc = ((const CommonTableExpr *) expr)->location;
			break;
		case T_PlaceHolderVar:
			/* just use argument's location */
			loc = exprLocation((Node *) ((const PlaceHolderVar *) expr)->phexpr);
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
 * caller.	This can be used to short-circuit the traversal if the walker
 * has found what it came for.	"false" is returned to the top-level caller
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
 * the outer plan).  It will not touch the completed subplan, however.	Since
 * there is no link to the original Query, it is not possible to recurse into
 * subselects of an already-planned expression tree.  This is OK for current
 * uses, but may need to be revisited in future.
 */

bool
expression_tree_walker(Node *node,
					   bool (*walker) (),
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
	if (node == NULL)
		return false;

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	switch (nodeTag(node))
	{
		case T_Var:
		case T_Const:
		case T_Param:
		case T_CoerceToDomainValue:
		case T_CaseTestExpr:
		case T_SetToDefault:
		case T_CurrentOfExpr:
		case T_RangeTblRef:
		case T_SortGroupClause:
			/* primitive node types with no expression subnodes */
			break;
		case T_Aggref:
			{
				Aggref	   *expr = (Aggref *) node;

				/* recurse directly on List */
				if (expression_tree_walker((Node *) expr->args,
										   walker, context))
					return true;
				if (expression_tree_walker((Node *) expr->aggorder,
										   walker, context))
					return true;
				if (expression_tree_walker((Node *) expr->aggdistinct,
										   walker, context))
					return true;
			}
			break;
		case T_WindowFunc:
			{
				WindowFunc *expr = (WindowFunc *) node;

				/* recurse directly on List */
				if (expression_tree_walker((Node *) expr->args,
										   walker, context))
					return true;
			}
			break;
		case T_ArrayRef:
			{
				ArrayRef   *aref = (ArrayRef *) node;

				/* recurse directly for upper/lower array index lists */
				if (expression_tree_walker((Node *) aref->refupperindexpr,
										   walker, context))
					return true;
				if (expression_tree_walker((Node *) aref->reflowerindexpr,
										   walker, context))
					return true;
				/* walker must see the refexpr and refassgnexpr, however */
				if (walker(aref->refexpr, context))
					return true;
				if (walker(aref->refassgnexpr, context))
					return true;
			}
			break;
		case T_FuncExpr:
			{
				FuncExpr   *expr = (FuncExpr *) node;

				if (expression_tree_walker((Node *) expr->args,
										   walker, context))
					return true;
			}
			break;
		case T_NamedArgExpr:
			return walker(((NamedArgExpr *) node)->arg, context);
		case T_OpExpr:
		case T_DistinctExpr:	/* struct-equivalent to OpExpr */
		case T_NullIfExpr:		/* struct-equivalent to OpExpr */
			{
				OpExpr	   *expr = (OpExpr *) node;

				if (expression_tree_walker((Node *) expr->args,
										   walker, context))
					return true;
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;

				if (expression_tree_walker((Node *) expr->args,
										   walker, context))
					return true;
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;

				if (expression_tree_walker((Node *) expr->args,
										   walker, context))
					return true;
			}
			break;
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;

				if (walker(sublink->testexpr, context))
					return true;

				/*
				 * Also invoke the walker on the sublink's Query node, so it
				 * can recurse into the sub-query if it wants to.
				 */
				return walker(sublink->subselect, context);
			}
			break;
		case T_SubPlan:
			{
				SubPlan    *subplan = (SubPlan *) node;

				/* recurse into the testexpr, but not into the Plan */
				if (walker(subplan->testexpr, context))
					return true;
				/* also examine args list */
				if (expression_tree_walker((Node *) subplan->args,
										   walker, context))
					return true;
			}
			break;
		case T_AlternativeSubPlan:
			return walker(((AlternativeSubPlan *) node)->subplans, context);
		case T_FieldSelect:
			return walker(((FieldSelect *) node)->arg, context);
		case T_FieldStore:
			{
				FieldStore *fstore = (FieldStore *) node;

				if (walker(fstore->arg, context))
					return true;
				if (walker(fstore->newvals, context))
					return true;
			}
			break;
		case T_RelabelType:
			return walker(((RelabelType *) node)->arg, context);
		case T_CoerceViaIO:
			return walker(((CoerceViaIO *) node)->arg, context);
		case T_ArrayCoerceExpr:
			return walker(((ArrayCoerceExpr *) node)->arg, context);
		case T_ConvertRowtypeExpr:
			return walker(((ConvertRowtypeExpr *) node)->arg, context);
		case T_CollateExpr:
			return walker(((CollateExpr *) node)->arg, context);
		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;

				if (walker(caseexpr->arg, context))
					return true;
				/* we assume walker doesn't care about CaseWhens, either */
				foreach(temp, caseexpr->args)
				{
					CaseWhen   *when = (CaseWhen *) lfirst(temp);

					Assert(IsA(when, CaseWhen));
					if (walker(when->expr, context))
						return true;
					if (walker(when->result, context))
						return true;
				}
				if (walker(caseexpr->defresult, context))
					return true;
			}
			break;
		case T_ArrayExpr:
			return walker(((ArrayExpr *) node)->elements, context);
		case T_RowExpr:
			/* Assume colnames isn't interesting */
			return walker(((RowExpr *) node)->args, context);
		case T_RowCompareExpr:
			{
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;

				if (walker(rcexpr->largs, context))
					return true;
				if (walker(rcexpr->rargs, context))
					return true;
			}
			break;
		case T_CoalesceExpr:
			return walker(((CoalesceExpr *) node)->args, context);
		case T_MinMaxExpr:
			return walker(((MinMaxExpr *) node)->args, context);
		case T_XmlExpr:
			{
				XmlExpr    *xexpr = (XmlExpr *) node;

				if (walker(xexpr->named_args, context))
					return true;
				/* we assume walker doesn't care about arg_names */
				if (walker(xexpr->args, context))
					return true;
			}
			break;
		case T_NullTest:
			return walker(((NullTest *) node)->arg, context);
		case T_BooleanTest:
			return walker(((BooleanTest *) node)->arg, context);
		case T_CoerceToDomain:
			return walker(((CoerceToDomain *) node)->arg, context);
		case T_TargetEntry:
			return walker(((TargetEntry *) node)->expr, context);
		case T_Query:
			/* Do nothing with a sub-Query, per discussion above */
			break;
		case T_WindowClause:
			{
				WindowClause *wc = (WindowClause *) node;

				if (walker(wc->partitionClause, context))
					return true;
				if (walker(wc->orderClause, context))
					return true;
				if (walker(wc->startOffset, context))
					return true;
				if (walker(wc->endOffset, context))
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
				return walker(cte->ctequery, context);
			}
			break;
		case T_List:
			foreach(temp, (List *) node)
			{
				if (walker((Node *) lfirst(temp), context))
					return true;
			}
			break;
		case T_FromExpr:
			{
				FromExpr   *from = (FromExpr *) node;

				if (walker(from->fromlist, context))
					return true;
				if (walker(from->quals, context))
					return true;
			}
			break;
		case T_JoinExpr:
			{
				JoinExpr   *join = (JoinExpr *) node;

				if (walker(join->larg, context))
					return true;
				if (walker(join->rarg, context))
					return true;
				if (walker(join->quals, context))
					return true;

				/*
				 * alias clause, using list are deemed uninteresting.
				 */
			}
			break;
		case T_SetOperationStmt:
			{
				SetOperationStmt *setop = (SetOperationStmt *) node;

				if (walker(setop->larg, context))
					return true;
				if (walker(setop->rarg, context))
					return true;

				/* groupClauses are deemed uninteresting */
			}
			break;
		case T_PlaceHolderVar:
			return walker(((PlaceHolderVar *) node)->phexpr, context);
		case T_AppendRelInfo:
			{
				AppendRelInfo *appinfo = (AppendRelInfo *) node;

				if (expression_tree_walker((Node *) appinfo->translated_vars,
										   walker, context))
					return true;
			}
			break;
		case T_PlaceHolderInfo:
			return walker(((PlaceHolderInfo *) node)->ph_var, context);
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(node));
			break;
	}
	return false;
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
 * which is the bitwise OR of flag values to suppress visitation of
 * indicated items.  (More flag bits may be added as needed.)
 */
bool
query_tree_walker(Query *query,
				  bool (*walker) (),
				  void *context,
				  int flags)
{
	Assert(query != NULL && IsA(query, Query));

	if (walker((Node *) query->targetList, context))
		return true;
	if (walker((Node *) query->returningList, context))
		return true;
	if (walker((Node *) query->jointree, context))
		return true;
	if (walker(query->setOperations, context))
		return true;
	if (walker(query->havingQual, context))
		return true;
	if (walker(query->limitOffset, context))
		return true;
	if (walker(query->limitCount, context))
		return true;
	if (!(flags & QTW_IGNORE_CTE_SUBQUERIES))
	{
		if (walker((Node *) query->cteList, context))
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
range_table_walker(List *rtable,
				   bool (*walker) (),
				   void *context,
				   int flags)
{
	ListCell   *rt;

	foreach(rt, rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

		/* For historical reasons, visiting RTEs is not the default */
		if (flags & QTW_EXAMINE_RTES)
			if (walker(rte, context))
				return true;

		switch (rte->rtekind)
		{
			case RTE_RELATION:
			case RTE_CTE:
				/* nothing to do */
				break;
			case RTE_SUBQUERY:
				if (!(flags & QTW_IGNORE_RT_SUBQUERIES))
					if (walker(rte->subquery, context))
						return true;
				break;
			case RTE_JOIN:
				if (!(flags & QTW_IGNORE_JOINALIASES))
					if (walker(rte->joinaliasvars, context))
						return true;
				break;
			case RTE_FUNCTION:
				if (walker(rte->funcexpr, context))
					return true;
				break;
			case RTE_VALUES:
				if (walker(rte->values_lists, context))
					return true;
				break;
		}
	}
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
expression_tree_mutator(Node *node,
						Node *(*mutator) (),
						void *context)
{
	/*
	 * The mutator has already decided not to modify the current node, but we
	 * must call the mutator for any sub-nodes.
	 */

#define FLATCOPY(newnode, node, nodetype)  \
	( (newnode) = (nodetype *) palloc(sizeof(nodetype)), \
	  memcpy((newnode), (node), sizeof(nodetype)) )

#define CHECKFLATCOPY(newnode, node, nodetype)	\
	( AssertMacro(IsA((node), nodetype)), \
	  (newnode) = (nodetype *) palloc(sizeof(nodetype)), \
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
		case T_CoerceToDomainValue:
		case T_CaseTestExpr:
		case T_SetToDefault:
		case T_CurrentOfExpr:
		case T_RangeTblRef:
		case T_SortGroupClause:
			return (Node *) copyObject(node);
		case T_Aggref:
			{
				Aggref	   *aggref = (Aggref *) node;
				Aggref	   *newnode;

				FLATCOPY(newnode, aggref, Aggref);
				MUTATE(newnode->args, aggref->args, List *);
				MUTATE(newnode->aggorder, aggref->aggorder, List *);
				MUTATE(newnode->aggdistinct, aggref->aggdistinct, List *);
				return (Node *) newnode;
			}
			break;
		case T_WindowFunc:
			{
				WindowFunc *wfunc = (WindowFunc *) node;
				WindowFunc *newnode;

				FLATCOPY(newnode, wfunc, WindowFunc);
				MUTATE(newnode->args, wfunc->args, List *);
				return (Node *) newnode;
			}
			break;
		case T_ArrayRef:
			{
				ArrayRef   *arrayref = (ArrayRef *) node;
				ArrayRef   *newnode;

				FLATCOPY(newnode, arrayref, ArrayRef);
				MUTATE(newnode->refupperindexpr, arrayref->refupperindexpr,
					   List *);
				MUTATE(newnode->reflowerindexpr, arrayref->reflowerindexpr,
					   List *);
				MUTATE(newnode->refexpr, arrayref->refexpr,
					   Expr *);
				MUTATE(newnode->refassgnexpr, arrayref->refassgnexpr,
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
		case T_PlaceHolderVar:
			{
				PlaceHolderVar *phv = (PlaceHolderVar *) node;
				PlaceHolderVar *newnode;

				FLATCOPY(newnode, phv, PlaceHolderVar);
				MUTATE(newnode->phexpr, phv->phexpr, Expr *);
				/* Assume we need not copy the relids bitmapset */
				return (Node *) newnode;
			}
			break;
		case T_AppendRelInfo:
			{
				AppendRelInfo *appinfo = (AppendRelInfo *) node;
				AppendRelInfo *newnode;

				FLATCOPY(newnode, appinfo, AppendRelInfo);
				MUTATE(newnode->translated_vars, appinfo->translated_vars, List *);
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
 * mutator intends to descend into subqueries.	It is also useful for
 * descending into subqueries within a mutator.
 *
 * Some callers want to suppress mutating of certain items in the Query,
 * typically because they need to process them specially, or don't actually
 * want to recurse into subqueries.  This is supported by the flags argument,
 * which is the bitwise OR of flag values to suppress mutating of
 * indicated items.  (More flag bits may be added as needed.)
 *
 * Normally the Query node itself is copied, but some callers want it to be
 * modified in-place; they must pass QTW_DONT_COPY_QUERY in flags.	All
 * modified substructure is safely copied in any case.
 */
Query *
query_tree_mutator(Query *query,
				   Node *(*mutator) (),
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
	MUTATE(query->returningList, query->returningList, List *);
	MUTATE(query->jointree, query->jointree, FromExpr *);
	MUTATE(query->setOperations, query->setOperations, Node *);
	MUTATE(query->havingQual, query->havingQual, Node *);
	MUTATE(query->limitOffset, query->limitOffset, Node *);
	MUTATE(query->limitCount, query->limitCount, Node *);
	if (!(flags & QTW_IGNORE_CTE_SUBQUERIES))
		MUTATE(query->cteList, query->cteList, List *);
	else	/* else copy CTE list as-is */
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
range_table_mutator(List *rtable,
					Node *(*mutator) (),
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
			case RTE_CTE:
				/* we don't bother to copy eref, aliases, etc; OK? */
				break;
			case RTE_SUBQUERY:
				if (!(flags & QTW_IGNORE_RT_SUBQUERIES))
				{
					CHECKFLATCOPY(newrte->subquery, rte->subquery, Query);
					MUTATE(newrte->subquery, newrte->subquery, Query *);
				}
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
				MUTATE(newrte->funcexpr, rte->funcexpr, Node *);
				break;
			case RTE_VALUES:
				MUTATE(newrte->values_lists, rte->values_lists, List *);
				break;
		}
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
query_or_expression_tree_walker(Node *node,
								bool (*walker) (),
								void *context,
								int flags)
{
	if (node && IsA(node, Query))
		return query_tree_walker((Query *) node,
								 walker,
								 context,
								 flags);
	else
		return walker(node, context);
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
query_or_expression_tree_mutator(Node *node,
								 Node *(*mutator) (),
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
 * Currently, the node type coverage extends to SelectStmt and everything
 * that could appear under it, but not other statement types.
 */
bool
raw_expression_tree_walker(Node *node,
						   bool (*walker) (),
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
		case T_SetToDefault:
		case T_CurrentOfExpr:
		case T_Integer:
		case T_Float:
		case T_String:
		case T_BitString:
		case T_Null:
		case T_ParamRef:
		case T_A_Const:
		case T_A_Star:
			/* primitive node types with no subnodes */
			break;
		case T_Alias:
			/* we assume the colnames list isn't interesting */
			break;
		case T_RangeVar:
			return walker(((RangeVar *) node)->alias, context);
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;

				if (walker(sublink->testexpr, context))
					return true;
				/* we assume the operName is not interesting */
				if (walker(sublink->subselect, context))
					return true;
			}
			break;
		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;

				if (walker(caseexpr->arg, context))
					return true;
				/* we assume walker doesn't care about CaseWhens, either */
				foreach(temp, caseexpr->args)
				{
					CaseWhen   *when = (CaseWhen *) lfirst(temp);

					Assert(IsA(when, CaseWhen));
					if (walker(when->expr, context))
						return true;
					if (walker(when->result, context))
						return true;
				}
				if (walker(caseexpr->defresult, context))
					return true;
			}
			break;
		case T_RowExpr:
			/* Assume colnames isn't interesting */
			return walker(((RowExpr *) node)->args, context);
		case T_CoalesceExpr:
			return walker(((CoalesceExpr *) node)->args, context);
		case T_MinMaxExpr:
			return walker(((MinMaxExpr *) node)->args, context);
		case T_XmlExpr:
			{
				XmlExpr    *xexpr = (XmlExpr *) node;

				if (walker(xexpr->named_args, context))
					return true;
				/* we assume walker doesn't care about arg_names */
				if (walker(xexpr->args, context))
					return true;
			}
			break;
		case T_NullTest:
			return walker(((NullTest *) node)->arg, context);
		case T_BooleanTest:
			return walker(((BooleanTest *) node)->arg, context);
		case T_JoinExpr:
			{
				JoinExpr   *join = (JoinExpr *) node;

				if (walker(join->larg, context))
					return true;
				if (walker(join->rarg, context))
					return true;
				if (walker(join->quals, context))
					return true;
				if (walker(join->alias, context))
					return true;
				/* using list is deemed uninteresting */
			}
			break;
		case T_IntoClause:
			{
				IntoClause *into = (IntoClause *) node;

				if (walker(into->rel, context))
					return true;
				/* colNames, options are deemed uninteresting */
				/* viewQuery should be null in raw parsetree, but check it */
				if (walker(into->viewQuery, context))
					return true;
			}
			break;
		case T_List:
			foreach(temp, (List *) node)
			{
				if (walker((Node *) lfirst(temp), context))
					return true;
			}
			break;
		case T_InsertStmt:
			{
				InsertStmt *stmt = (InsertStmt *) node;

				if (walker(stmt->relation, context))
					return true;
				if (walker(stmt->cols, context))
					return true;
				if (walker(stmt->selectStmt, context))
					return true;
				if (walker(stmt->returningList, context))
					return true;
				if (walker(stmt->withClause, context))
					return true;
			}
			break;
		case T_DeleteStmt:
			{
				DeleteStmt *stmt = (DeleteStmt *) node;

				if (walker(stmt->relation, context))
					return true;
				if (walker(stmt->usingClause, context))
					return true;
				if (walker(stmt->whereClause, context))
					return true;
				if (walker(stmt->returningList, context))
					return true;
				if (walker(stmt->withClause, context))
					return true;
			}
			break;
		case T_UpdateStmt:
			{
				UpdateStmt *stmt = (UpdateStmt *) node;

				if (walker(stmt->relation, context))
					return true;
				if (walker(stmt->targetList, context))
					return true;
				if (walker(stmt->whereClause, context))
					return true;
				if (walker(stmt->fromClause, context))
					return true;
				if (walker(stmt->returningList, context))
					return true;
				if (walker(stmt->withClause, context))
					return true;
			}
			break;
		case T_SelectStmt:
			{
				SelectStmt *stmt = (SelectStmt *) node;

				if (walker(stmt->distinctClause, context))
					return true;
				if (walker(stmt->intoClause, context))
					return true;
				if (walker(stmt->targetList, context))
					return true;
				if (walker(stmt->fromClause, context))
					return true;
				if (walker(stmt->whereClause, context))
					return true;
				if (walker(stmt->groupClause, context))
					return true;
				if (walker(stmt->havingClause, context))
					return true;
				if (walker(stmt->windowClause, context))
					return true;
				if (walker(stmt->valuesLists, context))
					return true;
				if (walker(stmt->sortClause, context))
					return true;
				if (walker(stmt->limitOffset, context))
					return true;
				if (walker(stmt->limitCount, context))
					return true;
				if (walker(stmt->lockingClause, context))
					return true;
				if (walker(stmt->withClause, context))
					return true;
				if (walker(stmt->larg, context))
					return true;
				if (walker(stmt->rarg, context))
					return true;
			}
			break;
		case T_A_Expr:
			{
				A_Expr	   *expr = (A_Expr *) node;

				if (walker(expr->lexpr, context))
					return true;
				if (walker(expr->rexpr, context))
					return true;
				/* operator name is deemed uninteresting */
			}
			break;
		case T_ColumnRef:
			/* we assume the fields contain nothing interesting */
			break;
		case T_FuncCall:
			{
				FuncCall   *fcall = (FuncCall *) node;

				if (walker(fcall->args, context))
					return true;
				if (walker(fcall->agg_order, context))
					return true;
				if (walker(fcall->over, context))
					return true;
				/* function name is deemed uninteresting */
			}
			break;
		case T_NamedArgExpr:
			return walker(((NamedArgExpr *) node)->arg, context);
		case T_A_Indices:
			{
				A_Indices  *indices = (A_Indices *) node;

				if (walker(indices->lidx, context))
					return true;
				if (walker(indices->uidx, context))
					return true;
			}
			break;
		case T_A_Indirection:
			{
				A_Indirection *indir = (A_Indirection *) node;

				if (walker(indir->arg, context))
					return true;
				if (walker(indir->indirection, context))
					return true;
			}
			break;
		case T_A_ArrayExpr:
			return walker(((A_ArrayExpr *) node)->elements, context);
		case T_ResTarget:
			{
				ResTarget  *rt = (ResTarget *) node;

				if (walker(rt->indirection, context))
					return true;
				if (walker(rt->val, context))
					return true;
			}
			break;
		case T_TypeCast:
			{
				TypeCast   *tc = (TypeCast *) node;

				if (walker(tc->arg, context))
					return true;
				if (walker(tc->typeName, context))
					return true;
			}
			break;
		case T_CollateClause:
			return walker(((CollateClause *) node)->arg, context);
		case T_SortBy:
			return walker(((SortBy *) node)->node, context);
		case T_WindowDef:
			{
				WindowDef  *wd = (WindowDef *) node;

				if (walker(wd->partitionClause, context))
					return true;
				if (walker(wd->orderClause, context))
					return true;
				if (walker(wd->startOffset, context))
					return true;
				if (walker(wd->endOffset, context))
					return true;
			}
			break;
		case T_RangeSubselect:
			{
				RangeSubselect *rs = (RangeSubselect *) node;

				if (walker(rs->subquery, context))
					return true;
				if (walker(rs->alias, context))
					return true;
			}
			break;
		case T_RangeFunction:
			{
				RangeFunction *rf = (RangeFunction *) node;

				if (walker(rf->funccallnode, context))
					return true;
				if (walker(rf->alias, context))
					return true;
			}
			break;
		case T_TypeName:
			{
				TypeName   *tn = (TypeName *) node;

				if (walker(tn->typmods, context))
					return true;
				if (walker(tn->arrayBounds, context))
					return true;
				/* type name itself is deemed uninteresting */
			}
			break;
		case T_ColumnDef:
			{
				ColumnDef  *coldef = (ColumnDef *) node;

				if (walker(coldef->typeName, context))
					return true;
				if (walker(coldef->raw_default, context))
					return true;
				if (walker(coldef->collClause, context))
					return true;
				/* for now, constraints are ignored */
			}
			break;
		case T_LockingClause:
			return walker(((LockingClause *) node)->lockedRels, context);
		case T_XmlSerialize:
			{
				XmlSerialize *xs = (XmlSerialize *) node;

				if (walker(xs->expr, context))
					return true;
				if (walker(xs->typeName, context))
					return true;
			}
			break;
		case T_WithClause:
			return walker(((WithClause *) node)->ctes, context);
		case T_CommonTableExpr:
			return walker(((CommonTableExpr *) node)->ctequery, context);
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(node));
			break;
	}
	return false;
}
