/*-------------------------------------------------------------------------
 *
 * parse_collate.c
 *		Routines for assigning collation information.
 *
 * We choose to handle collation analysis in a post-pass over the output
 * of expression parse analysis.  This is because we need more state to
 * perform this processing than is needed in the finished tree.  If we
 * did it on-the-fly while building the tree, all that state would have
 * to be kept in expression node trees permanently.  This way, the extra
 * storage is just local variables in this recursive routine.
 *
 * The info that is actually saved in the finished tree is:
 * 1. The output collation of each expression node, or InvalidOid if it
 * returns a noncollatable data type.  This can also be InvalidOid if the
 * result type is collatable but the collation is indeterminate.
 * 2. The collation to be used in executing each function.  InvalidOid means
 * that there are no collatable inputs or their collation is indeterminate.
 * This value is only stored in node types that might call collation-using
 * functions.
 *
 * You might think we could get away with storing only one collation per
 * node, but the two concepts really need to be kept distinct.  Otherwise
 * it's too confusing when a function produces a collatable output type but
 * has no collatable inputs or produces noncollatable output from collatable
 * inputs.
 *
 * Cases with indeterminate collation might result in an error being thrown
 * at runtime.  If we knew exactly which functions require collation
 * information, we could throw those errors at parse time instead.
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_collate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "catalog/pg_collation.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_collate.h"
#include "utils/lsyscache.h"


/*
 * Collation strength (the SQL standard calls this "derivation").  Order is
 * chosen to allow comparisons to work usefully.  Note: the standard doesn't
 * seem to distinguish between NONE and CONFLICT.
 */
typedef enum
{
	COLLATE_NONE,				/* expression is of a noncollatable datatype */
	COLLATE_IMPLICIT,			/* collation was derived implicitly */
	COLLATE_CONFLICT,			/* we had a conflict of implicit collations */
	COLLATE_EXPLICIT			/* collation was derived explicitly */
} CollateStrength;

typedef struct
{
	ParseState *pstate;			/* parse state (for error reporting) */
	Oid			collation;		/* OID of current collation, if any */
	CollateStrength strength;	/* strength of current collation choice */
	int			location;		/* location of expr that set collation */
	/* Remaining fields are only valid when strength == COLLATE_CONFLICT */
	Oid			collation2;		/* OID of conflicting collation */
	int			location2;		/* location of expr that set collation2 */
} assign_collations_context;

static bool assign_query_collations_walker(Node *node, ParseState *pstate);
static bool assign_collations_walker(Node *node,
						 assign_collations_context *context);
static void merge_collation_state(Oid collation,
					  CollateStrength strength,
					  int location,
					  Oid collation2,
					  int location2,
					  assign_collations_context *context);
static void assign_aggregate_collations(Aggref *aggref,
							assign_collations_context *loccontext);
static void assign_ordered_set_collations(Aggref *aggref,
							  assign_collations_context *loccontext);
static void assign_hypothetical_collations(Aggref *aggref,
							   assign_collations_context *loccontext);


/*
 * assign_query_collations()
 *		Mark all expressions in the given Query with collation information.
 *
 * This should be applied to each Query after completion of parse analysis
 * for expressions.  Note that we do not recurse into sub-Queries, since
 * those should have been processed when built.
 */
void
assign_query_collations(ParseState *pstate, Query *query)
{
	/*
	 * We just use query_tree_walker() to visit all the contained expressions.
	 * We can skip the rangetable and CTE subqueries, though, since RTEs and
	 * subqueries had better have been processed already (else Vars referring
	 * to them would not get created with the right collation).
	 */
	(void) query_tree_walker(query,
							 assign_query_collations_walker,
							 (void *) pstate,
							 QTW_IGNORE_RANGE_TABLE |
							 QTW_IGNORE_CTE_SUBQUERIES);
}

/*
 * Walker for assign_query_collations
 *
 * Each expression found by query_tree_walker is processed independently.
 * Note that query_tree_walker may pass us a whole List, such as the
 * targetlist, in which case each subexpression must be processed
 * independently --- we don't want to bleat if two different targetentries
 * have different collations.
 */
static bool
assign_query_collations_walker(Node *node, ParseState *pstate)
{
	/* Need do nothing for empty subexpressions */
	if (node == NULL)
		return false;

	/*
	 * We don't want to recurse into a set-operations tree; it's already been
	 * fully processed in transformSetOperationStmt.
	 */
	if (IsA(node, SetOperationStmt))
		return false;

	if (IsA(node, List))
		assign_list_collations(pstate, (List *) node);
	else
		assign_expr_collations(pstate, node);

	return false;
}

/*
 * assign_list_collations()
 *		Mark all nodes in the list of expressions with collation information.
 *
 * The list member expressions are processed independently; they do not have
 * to share a common collation.
 */
void
assign_list_collations(ParseState *pstate, List *exprs)
{
	ListCell   *lc;

	foreach(lc, exprs)
	{
		Node	   *node = (Node *) lfirst(lc);

		assign_expr_collations(pstate, node);
	}
}

/*
 * assign_expr_collations()
 *		Mark all nodes in the given expression tree with collation information.
 *
 * This is exported for the benefit of various utility commands that process
 * expressions without building a complete Query.  It should be applied after
 * calling transformExpr() plus any expression-modifying operations such as
 * coerce_to_boolean().
 */
void
assign_expr_collations(ParseState *pstate, Node *expr)
{
	assign_collations_context context;

	/* initialize context for tree walk */
	context.pstate = pstate;
	context.collation = InvalidOid;
	context.strength = COLLATE_NONE;
	context.location = -1;

	/* and away we go */
	(void) assign_collations_walker(expr, &context);
}

/*
 * select_common_collation()
 *		Identify a common collation for a list of expressions.
 *
 * The expressions should all return the same datatype, else this is not
 * terribly meaningful.
 *
 * none_ok means that it is permitted to return InvalidOid, indicating that
 * no common collation could be identified, even for collatable datatypes.
 * Otherwise, an error is thrown for conflict of implicit collations.
 *
 * In theory, none_ok = true reflects the rules of SQL standard clause "Result
 * of data type combinations", none_ok = false reflects the rules of clause
 * "Collation determination" (in some cases invoked via "Grouping
 * operations").
 */
Oid
select_common_collation(ParseState *pstate, List *exprs, bool none_ok)
{
	assign_collations_context context;

	/* initialize context for tree walk */
	context.pstate = pstate;
	context.collation = InvalidOid;
	context.strength = COLLATE_NONE;
	context.location = -1;

	/* and away we go */
	(void) assign_collations_walker((Node *) exprs, &context);

	/* deal with collation conflict */
	if (context.strength == COLLATE_CONFLICT)
	{
		if (none_ok)
			return InvalidOid;
		ereport(ERROR,
				(errcode(ERRCODE_COLLATION_MISMATCH),
				 errmsg("collation mismatch between implicit collations \"%s\" and \"%s\"",
						get_collation_name(context.collation),
						get_collation_name(context.collation2)),
				 errhint("You can choose the collation by applying the COLLATE clause to one or both expressions."),
				 parser_errposition(context.pstate, context.location2)));
	}

	/*
	 * Note: if strength is still COLLATE_NONE, we'll return InvalidOid, but
	 * that's okay because it must mean none of the expressions returned
	 * collatable datatypes.
	 */
	return context.collation;
}

/*
 * assign_collations_walker()
 *		Recursive guts of collation processing.
 *
 * Nodes with no children (eg, Vars, Consts, Params) must have been marked
 * when built.  All upper-level nodes are marked here.
 *
 * Note: if this is invoked directly on a List, it will attempt to infer a
 * common collation for all the list members.  In particular, it will throw
 * error if there are conflicting explicit collations for different members.
 */
static bool
assign_collations_walker(Node *node, assign_collations_context *context)
{
	assign_collations_context loccontext;
	Oid			collation;
	CollateStrength strength;
	int			location;

	/* Need do nothing for empty subexpressions */
	if (node == NULL)
		return false;

	/*
	 * Prepare for recursion.  For most node types, though not all, the first
	 * thing we do is recurse to process all nodes below this one. Each level
	 * of the tree has its own local context.
	 */
	loccontext.pstate = context->pstate;
	loccontext.collation = InvalidOid;
	loccontext.strength = COLLATE_NONE;
	loccontext.location = -1;
	/* Set these fields just to suppress uninitialized-value warnings: */
	loccontext.collation2 = InvalidOid;
	loccontext.location2 = -1;

	/*
	 * Recurse if appropriate, then determine the collation for this node.
	 *
	 * Note: the general cases are at the bottom of the switch, after various
	 * special cases.
	 */
	switch (nodeTag(node))
	{
		case T_CollateExpr:
			{
				/*
				 * COLLATE sets an explicitly derived collation, regardless of
				 * what the child state is.  But we must recurse to set up
				 * collation info below here.
				 */
				CollateExpr *expr = (CollateExpr *) node;

				(void) expression_tree_walker(node,
											  assign_collations_walker,
											  (void *) &loccontext);

				collation = expr->collOid;
				Assert(OidIsValid(collation));
				strength = COLLATE_EXPLICIT;
				location = expr->location;
			}
			break;
		case T_FieldSelect:
			{
				/*
				 * For FieldSelect, the result has the field's declared
				 * collation, independently of what happened in the arguments.
				 * (The immediate argument must be composite and thus not
				 * collatable, anyhow.)  The field's collation was already
				 * looked up and saved in the node.
				 */
				FieldSelect *expr = (FieldSelect *) node;

				/* ... but first, recurse */
				(void) expression_tree_walker(node,
											  assign_collations_walker,
											  (void *) &loccontext);

				if (OidIsValid(expr->resultcollid))
				{
					/* Node's result type is collatable. */
					/* Pass up field's collation as an implicit choice. */
					collation = expr->resultcollid;
					strength = COLLATE_IMPLICIT;
					location = exprLocation(node);
				}
				else
				{
					/* Node's result type isn't collatable. */
					collation = InvalidOid;
					strength = COLLATE_NONE;
					location = -1;		/* won't be used */
				}
			}
			break;
		case T_RowExpr:
			{
				/*
				 * RowExpr is a special case because the subexpressions are
				 * independent: we don't want to complain if some of them have
				 * incompatible explicit collations.
				 */
				RowExpr    *expr = (RowExpr *) node;

				assign_list_collations(context->pstate, expr->args);

				/*
				 * Since the result is always composite and therefore never
				 * has a collation, we can just stop here: this node has no
				 * impact on the collation of its parent.
				 */
				return false;	/* done */
			}
		case T_RowCompareExpr:
			{
				/*
				 * For RowCompare, we have to find the common collation of
				 * each pair of input columns and build a list.  If we can't
				 * find a common collation, we just put InvalidOid into the
				 * list, which may or may not cause an error at runtime.
				 */
				RowCompareExpr *expr = (RowCompareExpr *) node;
				List	   *colls = NIL;
				ListCell   *l;
				ListCell   *r;

				forboth(l, expr->largs, r, expr->rargs)
				{
					Node	   *le = (Node *) lfirst(l);
					Node	   *re = (Node *) lfirst(r);
					Oid			coll;

					coll = select_common_collation(context->pstate,
												   list_make2(le, re),
												   true);
					colls = lappend_oid(colls, coll);
				}
				expr->inputcollids = colls;

				/*
				 * Since the result is always boolean and therefore never has
				 * a collation, we can just stop here: this node has no impact
				 * on the collation of its parent.
				 */
				return false;	/* done */
			}
		case T_CoerceToDomain:
			{
				/*
				 * If the domain declaration included a non-default COLLATE
				 * spec, then use that collation as the output collation of
				 * the coercion.  Otherwise allow the input collation to
				 * bubble up.  (The input should be of the domain's base type,
				 * therefore we don't need to worry about it not being
				 * collatable when the domain is.)
				 */
				CoerceToDomain *expr = (CoerceToDomain *) node;
				Oid			typcollation = get_typcollation(expr->resulttype);

				/* ... but first, recurse */
				(void) expression_tree_walker(node,
											  assign_collations_walker,
											  (void *) &loccontext);

				if (OidIsValid(typcollation))
				{
					/* Node's result type is collatable. */
					if (typcollation == DEFAULT_COLLATION_OID)
					{
						/* Collation state bubbles up from child. */
						collation = loccontext.collation;
						strength = loccontext.strength;
						location = loccontext.location;
					}
					else
					{
						/* Use domain's collation as an implicit choice. */
						collation = typcollation;
						strength = COLLATE_IMPLICIT;
						location = exprLocation(node);
					}
				}
				else
				{
					/* Node's result type isn't collatable. */
					collation = InvalidOid;
					strength = COLLATE_NONE;
					location = -1;		/* won't be used */
				}

				/*
				 * Save the state into the expression node.  We know it
				 * doesn't care about input collation.
				 */
				if (strength == COLLATE_CONFLICT)
					exprSetCollation(node, InvalidOid);
				else
					exprSetCollation(node, collation);
			}
			break;
		case T_TargetEntry:
			(void) expression_tree_walker(node,
										  assign_collations_walker,
										  (void *) &loccontext);

			/*
			 * TargetEntry can have only one child, and should bubble that
			 * state up to its parent.  We can't use the general-case code
			 * below because exprType and friends don't work on TargetEntry.
			 */
			collation = loccontext.collation;
			strength = loccontext.strength;
			location = loccontext.location;

			/*
			 * Throw error if the collation is indeterminate for a TargetEntry
			 * that is a sort/group target.  We prefer to do this now, instead
			 * of leaving the comparison functions to fail at runtime, because
			 * we can give a syntax error pointer to help locate the problem.
			 * There are some cases where there might not be a failure, for
			 * example if the planner chooses to use hash aggregation instead
			 * of sorting for grouping; but it seems better to predictably
			 * throw an error.  (Compare transformSetOperationTree, which will
			 * throw error for indeterminate collation of set-op columns, even
			 * though the planner might be able to implement the set-op
			 * without sorting.)
			 */
			if (strength == COLLATE_CONFLICT &&
				((TargetEntry *) node)->ressortgroupref != 0)
				ereport(ERROR,
						(errcode(ERRCODE_COLLATION_MISMATCH),
						 errmsg("collation mismatch between implicit collations \"%s\" and \"%s\"",
								get_collation_name(loccontext.collation),
								get_collation_name(loccontext.collation2)),
						 errhint("You can choose the collation by applying the COLLATE clause to one or both expressions."),
						 parser_errposition(context->pstate,
											loccontext.location2)));
			break;
		case T_RangeTblRef:
		case T_JoinExpr:
		case T_FromExpr:
		case T_SortGroupClause:
			(void) expression_tree_walker(node,
										  assign_collations_walker,
										  (void *) &loccontext);

			/*
			 * When we're invoked on a query's jointree, we don't need to do
			 * anything with join nodes except recurse through them to process
			 * WHERE/ON expressions.  So just stop here.  Likewise, we don't
			 * need to do anything when invoked on sort/group lists.
			 */
			return false;
		case T_Query:
			{
				/*
				 * We get here when we're invoked on the Query belonging to a
				 * SubLink.  Act as though the Query returns its first output
				 * column, which indeed is what it does for EXPR_SUBLINK and
				 * ARRAY_SUBLINK cases.  In the cases where the SubLink
				 * returns boolean, this info will be ignored.  Special case:
				 * in EXISTS, the Query might return no columns, in which case
				 * we need do nothing.
				 *
				 * We needn't recurse, since the Query is already processed.
				 */
				Query	   *qtree = (Query *) node;
				TargetEntry *tent;

				if (qtree->targetList == NIL)
					return false;
				tent = (TargetEntry *) linitial(qtree->targetList);
				Assert(IsA(tent, TargetEntry));
				if (tent->resjunk)
					return false;

				collation = exprCollation((Node *) tent->expr);
				/* collation doesn't change if it's converted to array */
				strength = COLLATE_IMPLICIT;
				location = exprLocation((Node *) tent->expr);
			}
			break;
		case T_List:
			(void) expression_tree_walker(node,
										  assign_collations_walker,
										  (void *) &loccontext);

			/*
			 * When processing a list, collation state just bubbles up from
			 * the list elements.
			 */
			collation = loccontext.collation;
			strength = loccontext.strength;
			location = loccontext.location;
			break;

		case T_Var:
		case T_Const:
		case T_Param:
		case T_CoerceToDomainValue:
		case T_CaseTestExpr:
		case T_SetToDefault:
		case T_CurrentOfExpr:

			/*
			 * General case for childless expression nodes.  These should
			 * already have a collation assigned; it is not this function's
			 * responsibility to look into the catalogs for base-case
			 * information.
			 */
			collation = exprCollation(node);

			/*
			 * Note: in most cases, there will be an assigned collation
			 * whenever type_is_collatable(exprType(node)); but an exception
			 * occurs for a Var referencing a subquery output column for which
			 * a unique collation was not determinable.  That may lead to a
			 * runtime failure if a collation-sensitive function is applied to
			 * the Var.
			 */

			if (OidIsValid(collation))
				strength = COLLATE_IMPLICIT;
			else
				strength = COLLATE_NONE;
			location = exprLocation(node);
			break;

		default:
			{
				/*
				 * General case for most expression nodes with children. First
				 * recurse, then figure out what to assign to this node.
				 */
				Oid			typcollation;

				/*
				 * For most node types, we want to treat all the child
				 * expressions alike; but there are a few exceptions, hence
				 * this inner switch.
				 */
				switch (nodeTag(node))
				{
					case T_Aggref:
						{
							/*
							 * Aggref is messy enough that we give it its own
							 * function, in fact three of them.  The FILTER
							 * clause is independent of the rest of the
							 * aggregate, however, so it can be processed
							 * separately.
							 */
							Aggref	   *aggref = (Aggref *) node;

							switch (aggref->aggkind)
							{
								case AGGKIND_NORMAL:
									assign_aggregate_collations(aggref,
																&loccontext);
									break;
								case AGGKIND_ORDERED_SET:
									assign_ordered_set_collations(aggref,
																&loccontext);
									break;
								case AGGKIND_HYPOTHETICAL:
									assign_hypothetical_collations(aggref,
																&loccontext);
									break;
								default:
									elog(ERROR, "unrecognized aggkind: %d",
										 (int) aggref->aggkind);
							}

							assign_expr_collations(context->pstate,
												 (Node *) aggref->aggfilter);
						}
						break;
					case T_WindowFunc:
						{
							/*
							 * WindowFunc requires special processing only for
							 * its aggfilter clause, as for aggregates.
							 */
							WindowFunc *wfunc = (WindowFunc *) node;

							(void) assign_collations_walker((Node *) wfunc->args,
															&loccontext);

							assign_expr_collations(context->pstate,
												   (Node *) wfunc->aggfilter);
						}
						break;
					case T_CaseExpr:
						{
							/*
							 * CaseExpr is a special case because we do not
							 * want to recurse into the test expression (if
							 * any).  It was already marked with collations
							 * during transformCaseExpr, and furthermore its
							 * collation is not relevant to the result of the
							 * CASE --- only the output expressions are.
							 */
							CaseExpr   *expr = (CaseExpr *) node;
							ListCell   *lc;

							foreach(lc, expr->args)
							{
								CaseWhen   *when = (CaseWhen *) lfirst(lc);

								Assert(IsA(when, CaseWhen));

								/*
								 * The condition expressions mustn't affect
								 * the CASE's result collation either; but
								 * since they are known to yield boolean, it's
								 * safe to recurse directly on them --- they
								 * won't change loccontext.
								 */
								(void) assign_collations_walker((Node *) when->expr,
																&loccontext);
								(void) assign_collations_walker((Node *) when->result,
																&loccontext);
							}
							(void) assign_collations_walker((Node *) expr->defresult,
															&loccontext);
						}
						break;
					default:

						/*
						 * Normal case: all child expressions contribute
						 * equally to loccontext.
						 */
						(void) expression_tree_walker(node,
													assign_collations_walker,
													  (void *) &loccontext);
						break;
				}

				/*
				 * Now figure out what collation to assign to this node.
				 */
				typcollation = get_typcollation(exprType(node));
				if (OidIsValid(typcollation))
				{
					/* Node's result is collatable; what about its input? */
					if (loccontext.strength > COLLATE_NONE)
					{
						/* Collation state bubbles up from children. */
						collation = loccontext.collation;
						strength = loccontext.strength;
						location = loccontext.location;
					}
					else
					{
						/*
						 * Collatable output produced without any collatable
						 * input.  Use the type's collation (which is usually
						 * DEFAULT_COLLATION_OID, but might be different for a
						 * domain).
						 */
						collation = typcollation;
						strength = COLLATE_IMPLICIT;
						location = exprLocation(node);
					}
				}
				else
				{
					/* Node's result type isn't collatable. */
					collation = InvalidOid;
					strength = COLLATE_NONE;
					location = -1;		/* won't be used */
				}

				/*
				 * Save the result collation into the expression node. If the
				 * state is COLLATE_CONFLICT, we'll set the collation to
				 * InvalidOid, which might result in an error at runtime.
				 */
				if (strength == COLLATE_CONFLICT)
					exprSetCollation(node, InvalidOid);
				else
					exprSetCollation(node, collation);

				/*
				 * Likewise save the input collation, which is the one that
				 * any function called by this node should use.
				 */
				if (loccontext.strength == COLLATE_CONFLICT)
					exprSetInputCollation(node, InvalidOid);
				else
					exprSetInputCollation(node, loccontext.collation);
			}
			break;
	}

	/*
	 * Now, merge my information into my parent's state.
	 */
	merge_collation_state(collation,
						  strength,
						  location,
						  loccontext.collation2,
						  loccontext.location2,
						  context);

	return false;
}

/*
 * Merge collation state of a subexpression into the context for its parent.
 */
static void
merge_collation_state(Oid collation,
					  CollateStrength strength,
					  int location,
					  Oid collation2,
					  int location2,
					  assign_collations_context *context)
{
	/*
	 * If the collation strength for this node is different from what's
	 * already in *context, then this node either dominates or is dominated by
	 * earlier siblings.
	 */
	if (strength > context->strength)
	{
		/* Override previous parent state */
		context->collation = collation;
		context->strength = strength;
		context->location = location;
		/* Bubble up error info if applicable */
		if (strength == COLLATE_CONFLICT)
		{
			context->collation2 = collation2;
			context->location2 = location2;
		}
	}
	else if (strength == context->strength)
	{
		/* Merge, or detect error if there's a collation conflict */
		switch (strength)
		{
			case COLLATE_NONE:
				/* Nothing + nothing is still nothing */
				break;
			case COLLATE_IMPLICIT:
				if (collation != context->collation)
				{
					/*
					 * Non-default implicit collation always beats default.
					 */
					if (context->collation == DEFAULT_COLLATION_OID)
					{
						/* Override previous parent state */
						context->collation = collation;
						context->strength = strength;
						context->location = location;
					}
					else if (collation != DEFAULT_COLLATION_OID)
					{
						/*
						 * Ooops, we have a conflict.  We cannot throw error
						 * here, since the conflict could be resolved by a
						 * later sibling CollateExpr, or the parent might not
						 * care about collation anyway.  Return enough info to
						 * throw the error later, if needed.
						 */
						context->strength = COLLATE_CONFLICT;
						context->collation2 = collation;
						context->location2 = location;
					}
				}
				break;
			case COLLATE_CONFLICT:
				/* We're still conflicted ... */
				break;
			case COLLATE_EXPLICIT:
				if (collation != context->collation)
				{
					/*
					 * Ooops, we have a conflict of explicit COLLATE clauses.
					 * Here we choose to throw error immediately; that is what
					 * the SQL standard says to do, and there's no good reason
					 * to be less strict.
					 */
					ereport(ERROR,
							(errcode(ERRCODE_COLLATION_MISMATCH),
							 errmsg("collation mismatch between explicit collations \"%s\" and \"%s\"",
									get_collation_name(context->collation),
									get_collation_name(collation)),
							 parser_errposition(context->pstate, location)));
				}
				break;
		}
	}
}

/*
 * Aggref is a special case because expressions used only for ordering
 * shouldn't be taken to conflict with each other or with regular args,
 * indeed shouldn't affect the aggregate's result collation at all.
 * We handle this by applying assign_expr_collations() to them rather than
 * passing down our loccontext.
 *
 * Note that we recurse to each TargetEntry, not directly to its contained
 * expression, so that the case above for T_TargetEntry will complain if we
 * can't resolve a collation for an ORDER BY item (whether or not it is also
 * a normal aggregate arg).
 *
 * We need not recurse into the aggorder or aggdistinct lists, because those
 * contain only SortGroupClause nodes which we need not process.
 */
static void
assign_aggregate_collations(Aggref *aggref,
							assign_collations_context *loccontext)
{
	ListCell   *lc;

	/* Plain aggregates have no direct args */
	Assert(aggref->aggdirectargs == NIL);

	/* Process aggregated args, holding resjunk ones at arm's length */
	foreach(lc, aggref->args)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		Assert(IsA(tle, TargetEntry));
		if (tle->resjunk)
			assign_expr_collations(loccontext->pstate, (Node *) tle);
		else
			(void) assign_collations_walker((Node *) tle, loccontext);
	}
}

/*
 * For ordered-set aggregates, it's somewhat unclear how best to proceed.
 * The spec-defined inverse distribution functions have only one sort column
 * and don't return collatable types, but this is clearly too restrictive in
 * the general case.  Our solution is to consider that the aggregate's direct
 * arguments contribute normally to determination of the aggregate's own
 * collation, while aggregated arguments contribute only when the aggregate
 * is designed to have exactly one aggregated argument (i.e., it has a single
 * aggregated argument and is non-variadic).  If it can have more than one
 * aggregated argument, we process the aggregated arguments as independent
 * sort columns.  This avoids throwing error for something like
 *		agg(...) within group (order by x collate "foo", y collate "bar")
 * while also guaranteeing that variadic aggregates don't change in behavior
 * depending on how many sort columns a particular call happens to have.
 *
 * Otherwise this is much like the plain-aggregate case.
 */
static void
assign_ordered_set_collations(Aggref *aggref,
							  assign_collations_context *loccontext)
{
	bool		merge_sort_collations;
	ListCell   *lc;

	/* Merge sort collations to parent only if there can be only one */
	merge_sort_collations = (list_length(aggref->args) == 1 &&
					  get_func_variadictype(aggref->aggfnoid) == InvalidOid);

	/* Direct args, if any, are normal children of the Aggref node */
	(void) assign_collations_walker((Node *) aggref->aggdirectargs,
									loccontext);

	/* Process aggregated args appropriately */
	foreach(lc, aggref->args)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		Assert(IsA(tle, TargetEntry));
		if (merge_sort_collations)
			(void) assign_collations_walker((Node *) tle, loccontext);
		else
			assign_expr_collations(loccontext->pstate, (Node *) tle);
	}
}

/*
 * Hypothetical-set aggregates are even more special: per spec, we need to
 * unify the collations of each pair of hypothetical and aggregated args.
 * And we need to force the choice of collation down into the sort column
 * to ensure that the sort happens with the chosen collation.  Other than
 * that, the behavior is like regular ordered-set aggregates.  Note that
 * hypothetical direct arguments contribute to the aggregate collation
 * only when their partner aggregated arguments do.
 */
static void
assign_hypothetical_collations(Aggref *aggref,
							   assign_collations_context *loccontext)
{
	ListCell   *h_cell = list_head(aggref->aggdirectargs);
	ListCell   *s_cell = list_head(aggref->args);
	bool		merge_sort_collations;
	int			extra_args;

	/* Merge sort collations to parent only if there can be only one */
	merge_sort_collations = (list_length(aggref->args) == 1 &&
					  get_func_variadictype(aggref->aggfnoid) == InvalidOid);

	/* Process any non-hypothetical direct args */
	extra_args = list_length(aggref->aggdirectargs) - list_length(aggref->args);
	Assert(extra_args >= 0);
	while (extra_args-- > 0)
	{
		(void) assign_collations_walker((Node *) lfirst(h_cell), loccontext);
		h_cell = lnext(h_cell);
	}

	/* Scan hypothetical args and aggregated args in parallel */
	while (h_cell && s_cell)
	{
		Node	   *h_arg = (Node *) lfirst(h_cell);
		TargetEntry *s_tle = (TargetEntry *) lfirst(s_cell);
		assign_collations_context paircontext;

		/*
		 * Assign collations internally in this pair of expressions, then
		 * choose a common collation for them.  This should match
		 * select_common_collation(), but we can't use that function as-is
		 * because we need access to the whole collation state so we can
		 * bubble it up to the aggregate function's level.
		 */
		paircontext.pstate = loccontext->pstate;
		paircontext.collation = InvalidOid;
		paircontext.strength = COLLATE_NONE;
		paircontext.location = -1;
		/* Set these fields just to suppress uninitialized-value warnings: */
		paircontext.collation2 = InvalidOid;
		paircontext.location2 = -1;

		(void) assign_collations_walker(h_arg, &paircontext);
		(void) assign_collations_walker((Node *) s_tle->expr, &paircontext);

		/* deal with collation conflict */
		if (paircontext.strength == COLLATE_CONFLICT)
			ereport(ERROR,
					(errcode(ERRCODE_COLLATION_MISMATCH),
					 errmsg("collation mismatch between implicit collations \"%s\" and \"%s\"",
							get_collation_name(paircontext.collation),
							get_collation_name(paircontext.collation2)),
					 errhint("You can choose the collation by applying the COLLATE clause to one or both expressions."),
					 parser_errposition(paircontext.pstate,
										paircontext.location2)));

		/*
		 * At this point paircontext.collation can be InvalidOid only if the
		 * type is not collatable; no need to do anything in that case.  If we
		 * do have to change the sort column's collation, do it by inserting a
		 * RelabelType node into the sort column TLE.
		 *
		 * XXX This is pretty grotty for a couple of reasons:
		 * assign_collations_walker isn't supposed to be changing the
		 * expression structure like this, and a parse-time change of
		 * collation ought to be signaled by a CollateExpr not a RelabelType
		 * (the use of RelabelType for collation marking is supposed to be a
		 * planner/executor thing only).  But we have no better alternative.
		 * In particular, injecting a CollateExpr could result in the
		 * expression being interpreted differently after dump/reload, since
		 * we might be effectively promoting an implicit collation to
		 * explicit.  This kluge is relying on ruleutils.c not printing a
		 * COLLATE clause for a RelabelType, and probably on some other
		 * fragile behaviors.
		 */
		if (OidIsValid(paircontext.collation) &&
			paircontext.collation != exprCollation((Node *) s_tle->expr))
		{
			s_tle->expr = (Expr *)
				makeRelabelType(s_tle->expr,
								exprType((Node *) s_tle->expr),
								exprTypmod((Node *) s_tle->expr),
								paircontext.collation,
								COERCE_IMPLICIT_CAST);
		}

		/*
		 * If appropriate, merge this column's collation state up to the
		 * aggregate function.
		 */
		if (merge_sort_collations)
			merge_collation_state(paircontext.collation,
								  paircontext.strength,
								  paircontext.location,
								  paircontext.collation2,
								  paircontext.location2,
								  loccontext);

		h_cell = lnext(h_cell);
		s_cell = lnext(s_cell);
	}
	Assert(h_cell == NULL && s_cell == NULL);
}
