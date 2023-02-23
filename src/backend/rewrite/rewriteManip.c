/*-------------------------------------------------------------------------
 *
 * rewriteManip.c
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/rewrite/rewriteManip.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "parser/parse_coerce.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"


typedef struct
{
	int			sublevels_up;
} contain_aggs_of_level_context;

typedef struct
{
	int			agg_location;
	int			sublevels_up;
} locate_agg_of_level_context;

typedef struct
{
	int			win_location;
} locate_windowfunc_context;

static bool contain_aggs_of_level_walker(Node *node,
										 contain_aggs_of_level_context *context);
static bool locate_agg_of_level_walker(Node *node,
									   locate_agg_of_level_context *context);
static bool contain_windowfuncs_walker(Node *node, void *context);
static bool locate_windowfunc_walker(Node *node,
									 locate_windowfunc_context *context);
static bool checkExprHasSubLink_walker(Node *node, void *context);
static Relids offset_relid_set(Relids relids, int offset);
static Relids adjust_relid_set(Relids relids, int oldrelid, int newrelid);


/*
 * contain_aggs_of_level -
 *	Check if an expression contains an aggregate function call of a
 *	specified query level.
 *
 * The objective of this routine is to detect whether there are aggregates
 * belonging to the given query level.  Aggregates belonging to subqueries
 * or outer queries do NOT cause a true result.  We must recurse into
 * subqueries to detect outer-reference aggregates that logically belong to
 * the specified query level.
 */
bool
contain_aggs_of_level(Node *node, int levelsup)
{
	contain_aggs_of_level_context context;

	context.sublevels_up = levelsup;

	/*
	 * Must be prepared to start with a Query or a bare expression tree; if
	 * it's a Query, we don't want to increment sublevels_up.
	 */
	return query_or_expression_tree_walker(node,
										   contain_aggs_of_level_walker,
										   (void *) &context,
										   0);
}

static bool
contain_aggs_of_level_walker(Node *node,
							 contain_aggs_of_level_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		if (((Aggref *) node)->agglevelsup == context->sublevels_up)
			return true;		/* abort the tree traversal and return true */
		/* else fall through to examine argument */
	}
	if (IsA(node, GroupingFunc))
	{
		if (((GroupingFunc *) node)->agglevelsup == context->sublevels_up)
			return true;
		/* else fall through to examine argument */
	}
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   contain_aggs_of_level_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, contain_aggs_of_level_walker,
								  (void *) context);
}

/*
 * locate_agg_of_level -
 *	  Find the parse location of any aggregate of the specified query level.
 *
 * Returns -1 if no such agg is in the querytree, or if they all have
 * unknown parse location.  (The former case is probably caller error,
 * but we don't bother to distinguish it from the latter case.)
 *
 * Note: it might seem appropriate to merge this functionality into
 * contain_aggs_of_level, but that would complicate that function's API.
 * Currently, the only uses of this function are for error reporting,
 * and so shaving cycles probably isn't very important.
 */
int
locate_agg_of_level(Node *node, int levelsup)
{
	locate_agg_of_level_context context;

	context.agg_location = -1;	/* in case we find nothing */
	context.sublevels_up = levelsup;

	/*
	 * Must be prepared to start with a Query or a bare expression tree; if
	 * it's a Query, we don't want to increment sublevels_up.
	 */
	(void) query_or_expression_tree_walker(node,
										   locate_agg_of_level_walker,
										   (void *) &context,
										   0);

	return context.agg_location;
}

static bool
locate_agg_of_level_walker(Node *node,
						   locate_agg_of_level_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		if (((Aggref *) node)->agglevelsup == context->sublevels_up &&
			((Aggref *) node)->location >= 0)
		{
			context->agg_location = ((Aggref *) node)->location;
			return true;		/* abort the tree traversal and return true */
		}
		/* else fall through to examine argument */
	}
	if (IsA(node, GroupingFunc))
	{
		if (((GroupingFunc *) node)->agglevelsup == context->sublevels_up &&
			((GroupingFunc *) node)->location >= 0)
		{
			context->agg_location = ((GroupingFunc *) node)->location;
			return true;		/* abort the tree traversal and return true */
		}
	}
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   locate_agg_of_level_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, locate_agg_of_level_walker,
								  (void *) context);
}

/*
 * contain_windowfuncs -
 *	Check if an expression contains a window function call of the
 *	current query level.
 */
bool
contain_windowfuncs(Node *node)
{
	/*
	 * Must be prepared to start with a Query or a bare expression tree; if
	 * it's a Query, we don't want to increment sublevels_up.
	 */
	return query_or_expression_tree_walker(node,
										   contain_windowfuncs_walker,
										   NULL,
										   0);
}

static bool
contain_windowfuncs_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, WindowFunc))
		return true;			/* abort the tree traversal and return true */
	/* Mustn't recurse into subselects */
	return expression_tree_walker(node, contain_windowfuncs_walker,
								  (void *) context);
}

/*
 * locate_windowfunc -
 *	  Find the parse location of any windowfunc of the current query level.
 *
 * Returns -1 if no such windowfunc is in the querytree, or if they all have
 * unknown parse location.  (The former case is probably caller error,
 * but we don't bother to distinguish it from the latter case.)
 *
 * Note: it might seem appropriate to merge this functionality into
 * contain_windowfuncs, but that would complicate that function's API.
 * Currently, the only uses of this function are for error reporting,
 * and so shaving cycles probably isn't very important.
 */
int
locate_windowfunc(Node *node)
{
	locate_windowfunc_context context;

	context.win_location = -1;	/* in case we find nothing */

	/*
	 * Must be prepared to start with a Query or a bare expression tree; if
	 * it's a Query, we don't want to increment sublevels_up.
	 */
	(void) query_or_expression_tree_walker(node,
										   locate_windowfunc_walker,
										   (void *) &context,
										   0);

	return context.win_location;
}

static bool
locate_windowfunc_walker(Node *node, locate_windowfunc_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, WindowFunc))
	{
		if (((WindowFunc *) node)->location >= 0)
		{
			context->win_location = ((WindowFunc *) node)->location;
			return true;		/* abort the tree traversal and return true */
		}
		/* else fall through to examine argument */
	}
	/* Mustn't recurse into subselects */
	return expression_tree_walker(node, locate_windowfunc_walker,
								  (void *) context);
}

/*
 * checkExprHasSubLink -
 *	Check if an expression contains a SubLink.
 */
bool
checkExprHasSubLink(Node *node)
{
	/*
	 * If a Query is passed, examine it --- but we should not recurse into
	 * sub-Queries that are in its rangetable or CTE list.
	 */
	return query_or_expression_tree_walker(node,
										   checkExprHasSubLink_walker,
										   NULL,
										   QTW_IGNORE_RC_SUBQUERIES);
}

static bool
checkExprHasSubLink_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
		return true;			/* abort the tree traversal and return true */
	return expression_tree_walker(node, checkExprHasSubLink_walker, context);
}

/*
 * Check for MULTIEXPR Param within expression tree
 *
 * We intentionally don't descend into SubLinks: only Params at the current
 * query level are of interest.
 */
static bool
contains_multiexpr_param(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		if (((Param *) node)->paramkind == PARAM_MULTIEXPR)
			return true;		/* abort the tree traversal and return true */
		return false;
	}
	return expression_tree_walker(node, contains_multiexpr_param, context);
}


/*
 * OffsetVarNodes - adjust Vars when appending one query's RT to another
 *
 * Find all Var nodes in the given tree with varlevelsup == sublevels_up,
 * and increment their varno fields (rangetable indexes) by 'offset'.
 * The varnoold fields are adjusted similarly.  Also, adjust other nodes
 * that contain rangetable indexes, such as RangeTblRef and JoinExpr.
 *
 * NOTE: although this has the form of a walker, we cheat and modify the
 * nodes in-place.  The given expression tree should have been copied
 * earlier to ensure that no unwanted side-effects occur!
 */

typedef struct
{
	int			offset;
	int			sublevels_up;
} OffsetVarNodes_context;

static bool
OffsetVarNodes_walker(Node *node, OffsetVarNodes_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up)
		{
			var->varno += context->offset;
			var->varnoold += context->offset;
		}
		return false;
	}
	if (IsA(node, CurrentOfExpr))
	{
		CurrentOfExpr *cexpr = (CurrentOfExpr *) node;

		if (context->sublevels_up == 0)
			cexpr->cvarno += context->offset;
		return false;
	}
	if (IsA(node, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) node;

		if (context->sublevels_up == 0)
			rtr->rtindex += context->offset;
		/* the subquery itself is visited separately */
		return false;
	}
	if (IsA(node, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) node;

		if (j->rtindex && context->sublevels_up == 0)
			j->rtindex += context->offset;
		/* fall through to examine children */
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		if (phv->phlevelsup == context->sublevels_up)
		{
			phv->phrels = offset_relid_set(phv->phrels,
										   context->offset);
		}
		/* fall through to examine children */
	}
	if (IsA(node, AppendRelInfo))
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) node;

		if (context->sublevels_up == 0)
		{
			appinfo->parent_relid += context->offset;
			appinfo->child_relid += context->offset;
		}
		/* fall through to examine children */
	}
	/* Shouldn't need to handle other planner auxiliary nodes here */
	Assert(!IsA(node, PlanRowMark));
	Assert(!IsA(node, SpecialJoinInfo));
	Assert(!IsA(node, PlaceHolderInfo));
	Assert(!IsA(node, MinMaxAggInfo));

	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node, OffsetVarNodes_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, OffsetVarNodes_walker,
								  (void *) context);
}

void
OffsetVarNodes(Node *node, int offset, int sublevels_up)
{
	OffsetVarNodes_context context;

	context.offset = offset;
	context.sublevels_up = sublevels_up;

	/*
	 * Must be prepared to start with a Query or a bare expression tree; if
	 * it's a Query, go straight to query_tree_walker to make sure that
	 * sublevels_up doesn't get incremented prematurely.
	 */
	if (node && IsA(node, Query))
	{
		Query	   *qry = (Query *) node;

		/*
		 * If we are starting at a Query, and sublevels_up is zero, then we
		 * must also fix rangetable indexes in the Query itself --- namely
		 * resultRelation, exclRelIndex and rowMarks entries.  sublevels_up
		 * cannot be zero when recursing into a subquery, so there's no need
		 * to have the same logic inside OffsetVarNodes_walker.
		 */
		if (sublevels_up == 0)
		{
			ListCell   *l;

			if (qry->resultRelation)
				qry->resultRelation += offset;

			if (qry->onConflict && qry->onConflict->exclRelIndex)
				qry->onConflict->exclRelIndex += offset;

			foreach(l, qry->rowMarks)
			{
				RowMarkClause *rc = (RowMarkClause *) lfirst(l);

				rc->rti += offset;
			}
		}
		query_tree_walker(qry, OffsetVarNodes_walker,
						  (void *) &context, 0);
	}
	else
		OffsetVarNodes_walker(node, &context);
}

static Relids
offset_relid_set(Relids relids, int offset)
{
	Relids		result = NULL;
	int			rtindex;

	rtindex = -1;
	while ((rtindex = bms_next_member(relids, rtindex)) >= 0)
		result = bms_add_member(result, rtindex + offset);
	return result;
}

/*
 * ChangeVarNodes - adjust Var nodes for a specific change of RT index
 *
 * Find all Var nodes in the given tree belonging to a specific relation
 * (identified by sublevels_up and rt_index), and change their varno fields
 * to 'new_index'.  The varnoold fields are changed too.  Also, adjust other
 * nodes that contain rangetable indexes, such as RangeTblRef and JoinExpr.
 *
 * NOTE: although this has the form of a walker, we cheat and modify the
 * nodes in-place.  The given expression tree should have been copied
 * earlier to ensure that no unwanted side-effects occur!
 */

typedef struct
{
	int			rt_index;
	int			new_index;
	int			sublevels_up;
} ChangeVarNodes_context;

static bool
ChangeVarNodes_walker(Node *node, ChangeVarNodes_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up &&
			var->varno == context->rt_index)
		{
			var->varno = context->new_index;
			var->varnoold = context->new_index;
		}
		return false;
	}
	if (IsA(node, CurrentOfExpr))
	{
		CurrentOfExpr *cexpr = (CurrentOfExpr *) node;

		if (context->sublevels_up == 0 &&
			cexpr->cvarno == context->rt_index)
			cexpr->cvarno = context->new_index;
		return false;
	}
	if (IsA(node, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) node;

		if (context->sublevels_up == 0 &&
			rtr->rtindex == context->rt_index)
			rtr->rtindex = context->new_index;
		/* the subquery itself is visited separately */
		return false;
	}
	if (IsA(node, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) node;

		if (context->sublevels_up == 0 &&
			j->rtindex == context->rt_index)
			j->rtindex = context->new_index;
		/* fall through to examine children */
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		if (phv->phlevelsup == context->sublevels_up)
		{
			phv->phrels = adjust_relid_set(phv->phrels,
										   context->rt_index,
										   context->new_index);
		}
		/* fall through to examine children */
	}
	if (IsA(node, PlanRowMark))
	{
		PlanRowMark *rowmark = (PlanRowMark *) node;

		if (context->sublevels_up == 0)
		{
			if (rowmark->rti == context->rt_index)
				rowmark->rti = context->new_index;
			if (rowmark->prti == context->rt_index)
				rowmark->prti = context->new_index;
		}
		return false;
	}
	if (IsA(node, AppendRelInfo))
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) node;

		if (context->sublevels_up == 0)
		{
			if (appinfo->parent_relid == context->rt_index)
				appinfo->parent_relid = context->new_index;
			if (appinfo->child_relid == context->rt_index)
				appinfo->child_relid = context->new_index;
		}
		/* fall through to examine children */
	}
	/* Shouldn't need to handle other planner auxiliary nodes here */
	Assert(!IsA(node, SpecialJoinInfo));
	Assert(!IsA(node, PlaceHolderInfo));
	Assert(!IsA(node, MinMaxAggInfo));

	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node, ChangeVarNodes_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, ChangeVarNodes_walker,
								  (void *) context);
}

void
ChangeVarNodes(Node *node, int rt_index, int new_index, int sublevels_up)
{
	ChangeVarNodes_context context;

	context.rt_index = rt_index;
	context.new_index = new_index;
	context.sublevels_up = sublevels_up;

	/*
	 * Must be prepared to start with a Query or a bare expression tree; if
	 * it's a Query, go straight to query_tree_walker to make sure that
	 * sublevels_up doesn't get incremented prematurely.
	 */
	if (node && IsA(node, Query))
	{
		Query	   *qry = (Query *) node;

		/*
		 * If we are starting at a Query, and sublevels_up is zero, then we
		 * must also fix rangetable indexes in the Query itself --- namely
		 * resultRelation and rowMarks entries.  sublevels_up cannot be zero
		 * when recursing into a subquery, so there's no need to have the same
		 * logic inside ChangeVarNodes_walker.
		 */
		if (sublevels_up == 0)
		{
			ListCell   *l;

			if (qry->resultRelation == rt_index)
				qry->resultRelation = new_index;

			/* this is unlikely to ever be used, but ... */
			if (qry->onConflict && qry->onConflict->exclRelIndex == rt_index)
				qry->onConflict->exclRelIndex = new_index;

			foreach(l, qry->rowMarks)
			{
				RowMarkClause *rc = (RowMarkClause *) lfirst(l);

				if (rc->rti == rt_index)
					rc->rti = new_index;
			}
		}
		query_tree_walker(qry, ChangeVarNodes_walker,
						  (void *) &context, 0);
	}
	else
		ChangeVarNodes_walker(node, &context);
}

/*
 * Substitute newrelid for oldrelid in a Relid set
 */
static Relids
adjust_relid_set(Relids relids, int oldrelid, int newrelid)
{
	if (bms_is_member(oldrelid, relids))
	{
		/* Ensure we have a modifiable copy */
		relids = bms_copy(relids);
		/* Remove old, add new */
		relids = bms_del_member(relids, oldrelid);
		relids = bms_add_member(relids, newrelid);
	}
	return relids;
}

/*
 * IncrementVarSublevelsUp - adjust Var nodes when pushing them down in tree
 *
 * Find all Var nodes in the given tree having varlevelsup >= min_sublevels_up,
 * and add delta_sublevels_up to their varlevelsup value.  This is needed when
 * an expression that's correct for some nesting level is inserted into a
 * subquery.  Ordinarily the initial call has min_sublevels_up == 0 so that
 * all Vars are affected.  The point of min_sublevels_up is that we can
 * increment it when we recurse into a sublink, so that local variables in
 * that sublink are not affected, only outer references to vars that belong
 * to the expression's original query level or parents thereof.
 *
 * Likewise for other nodes containing levelsup fields, such as Aggref.
 *
 * NOTE: although this has the form of a walker, we cheat and modify the
 * Var nodes in-place.  The given expression tree should have been copied
 * earlier to ensure that no unwanted side-effects occur!
 */

typedef struct
{
	int			delta_sublevels_up;
	int			min_sublevels_up;
} IncrementVarSublevelsUp_context;

static bool
IncrementVarSublevelsUp_walker(Node *node,
							   IncrementVarSublevelsUp_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup >= context->min_sublevels_up)
			var->varlevelsup += context->delta_sublevels_up;
		return false;			/* done here */
	}
	if (IsA(node, CurrentOfExpr))
	{
		/* this should not happen */
		if (context->min_sublevels_up == 0)
			elog(ERROR, "cannot push down CurrentOfExpr");
		return false;
	}
	if (IsA(node, Aggref))
	{
		Aggref	   *agg = (Aggref *) node;

		if (agg->agglevelsup >= context->min_sublevels_up)
			agg->agglevelsup += context->delta_sublevels_up;
		/* fall through to recurse into argument */
	}
	if (IsA(node, GroupingFunc))
	{
		GroupingFunc *grp = (GroupingFunc *) node;

		if (grp->agglevelsup >= context->min_sublevels_up)
			grp->agglevelsup += context->delta_sublevels_up;
		/* fall through to recurse into argument */
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		if (phv->phlevelsup >= context->min_sublevels_up)
			phv->phlevelsup += context->delta_sublevels_up;
		/* fall through to recurse into argument */
	}
	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		if (rte->rtekind == RTE_CTE)
		{
			if (rte->ctelevelsup >= context->min_sublevels_up)
				rte->ctelevelsup += context->delta_sublevels_up;
		}
		return false;			/* allow range_table_walker to continue */
	}
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->min_sublevels_up++;
		result = query_tree_walker((Query *) node,
								   IncrementVarSublevelsUp_walker,
								   (void *) context,
								   QTW_EXAMINE_RTES_BEFORE);
		context->min_sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, IncrementVarSublevelsUp_walker,
								  (void *) context);
}

void
IncrementVarSublevelsUp(Node *node, int delta_sublevels_up,
						int min_sublevels_up)
{
	IncrementVarSublevelsUp_context context;

	context.delta_sublevels_up = delta_sublevels_up;
	context.min_sublevels_up = min_sublevels_up;

	/*
	 * Must be prepared to start with a Query or a bare expression tree; if
	 * it's a Query, we don't want to increment sublevels_up.
	 */
	query_or_expression_tree_walker(node,
									IncrementVarSublevelsUp_walker,
									(void *) &context,
									QTW_EXAMINE_RTES_BEFORE);
}

/*
 * IncrementVarSublevelsUp_rtable -
 *	Same as IncrementVarSublevelsUp, but to be invoked on a range table.
 */
void
IncrementVarSublevelsUp_rtable(List *rtable, int delta_sublevels_up,
							   int min_sublevels_up)
{
	IncrementVarSublevelsUp_context context;

	context.delta_sublevels_up = delta_sublevels_up;
	context.min_sublevels_up = min_sublevels_up;

	range_table_walker(rtable,
					   IncrementVarSublevelsUp_walker,
					   (void *) &context,
					   QTW_EXAMINE_RTES_BEFORE);
}


/*
 * rangeTableEntry_used - detect whether an RTE is referenced somewhere
 *	in var nodes or join or setOp trees of a query or expression.
 */

typedef struct
{
	int			rt_index;
	int			sublevels_up;
} rangeTableEntry_used_context;

static bool
rangeTableEntry_used_walker(Node *node,
							rangeTableEntry_used_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up &&
			var->varno == context->rt_index)
			return true;
		return false;
	}
	if (IsA(node, CurrentOfExpr))
	{
		CurrentOfExpr *cexpr = (CurrentOfExpr *) node;

		if (context->sublevels_up == 0 &&
			cexpr->cvarno == context->rt_index)
			return true;
		return false;
	}
	if (IsA(node, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) node;

		if (rtr->rtindex == context->rt_index &&
			context->sublevels_up == 0)
			return true;
		/* the subquery itself is visited separately */
		return false;
	}
	if (IsA(node, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) node;

		if (j->rtindex == context->rt_index &&
			context->sublevels_up == 0)
			return true;
		/* fall through to examine children */
	}
	/* Shouldn't need to handle planner auxiliary nodes here */
	Assert(!IsA(node, PlaceHolderVar));
	Assert(!IsA(node, PlanRowMark));
	Assert(!IsA(node, SpecialJoinInfo));
	Assert(!IsA(node, AppendRelInfo));
	Assert(!IsA(node, PlaceHolderInfo));
	Assert(!IsA(node, MinMaxAggInfo));

	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node, rangeTableEntry_used_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, rangeTableEntry_used_walker,
								  (void *) context);
}

bool
rangeTableEntry_used(Node *node, int rt_index, int sublevels_up)
{
	rangeTableEntry_used_context context;

	context.rt_index = rt_index;
	context.sublevels_up = sublevels_up;

	/*
	 * Must be prepared to start with a Query or a bare expression tree; if
	 * it's a Query, we don't want to increment sublevels_up.
	 */
	return query_or_expression_tree_walker(node,
										   rangeTableEntry_used_walker,
										   (void *) &context,
										   0);
}


/*
 * If the given Query is an INSERT ... SELECT construct, extract and
 * return the sub-Query node that represents the SELECT part.  Otherwise
 * return the given Query.
 *
 * If subquery_ptr is not NULL, then *subquery_ptr is set to the location
 * of the link to the SELECT subquery inside parsetree, or NULL if not an
 * INSERT ... SELECT.
 *
 * This is a hack needed because transformations on INSERT ... SELECTs that
 * appear in rule actions should be applied to the source SELECT, not to the
 * INSERT part.  Perhaps this can be cleaned up with redesigned querytrees.
 */
Query *
getInsertSelectQuery(Query *parsetree, Query ***subquery_ptr)
{
	Query	   *selectquery;
	RangeTblEntry *selectrte;
	RangeTblRef *rtr;

	if (subquery_ptr)
		*subquery_ptr = NULL;

	if (parsetree == NULL)
		return parsetree;
	if (parsetree->commandType != CMD_INSERT)
		return parsetree;

	/*
	 * Currently, this is ONLY applied to rule-action queries, and so we
	 * expect to find the OLD and NEW placeholder entries in the given query.
	 * If they're not there, it must be an INSERT/SELECT in which they've been
	 * pushed down to the SELECT.
	 */
	if (list_length(parsetree->rtable) >= 2 &&
		strcmp(rt_fetch(PRS2_OLD_VARNO, parsetree->rtable)->eref->aliasname,
			   "old") == 0 &&
		strcmp(rt_fetch(PRS2_NEW_VARNO, parsetree->rtable)->eref->aliasname,
			   "new") == 0)
		return parsetree;
	Assert(parsetree->jointree && IsA(parsetree->jointree, FromExpr));
	if (list_length(parsetree->jointree->fromlist) != 1)
		elog(ERROR, "expected to find SELECT subquery");
	rtr = (RangeTblRef *) linitial(parsetree->jointree->fromlist);
	if (!IsA(rtr, RangeTblRef))
		elog(ERROR, "expected to find SELECT subquery");
	selectrte = rt_fetch(rtr->rtindex, parsetree->rtable);
	if (!(selectrte->rtekind == RTE_SUBQUERY &&
		  selectrte->subquery &&
		  IsA(selectrte->subquery, Query) &&
		  selectrte->subquery->commandType == CMD_SELECT))
		elog(ERROR, "expected to find SELECT subquery");
	selectquery = selectrte->subquery;
	if (list_length(selectquery->rtable) >= 2 &&
		strcmp(rt_fetch(PRS2_OLD_VARNO, selectquery->rtable)->eref->aliasname,
			   "old") == 0 &&
		strcmp(rt_fetch(PRS2_NEW_VARNO, selectquery->rtable)->eref->aliasname,
			   "new") == 0)
	{
		if (subquery_ptr)
			*subquery_ptr = &(selectrte->subquery);
		return selectquery;
	}
	elog(ERROR, "could not find rule placeholders");
	return NULL;				/* not reached */
}


/*
 * Add the given qualifier condition to the query's WHERE clause
 */
void
AddQual(Query *parsetree, Node *qual)
{
	Node	   *copy;

	if (qual == NULL)
		return;

	if (parsetree->commandType == CMD_UTILITY)
	{
		/*
		 * There's noplace to put the qual on a utility statement.
		 *
		 * If it's a NOTIFY, silently ignore the qual; this means that the
		 * NOTIFY will execute, whether or not there are any qualifying rows.
		 * While clearly wrong, this is much more useful than refusing to
		 * execute the rule at all, and extra NOTIFY events are harmless for
		 * typical uses of NOTIFY.
		 *
		 * If it isn't a NOTIFY, error out, since unconditional execution of
		 * other utility stmts is unlikely to be wanted.  (This case is not
		 * currently allowed anyway, but keep the test for safety.)
		 */
		if (parsetree->utilityStmt && IsA(parsetree->utilityStmt, NotifyStmt))
			return;
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("conditional utility statements are not implemented")));
	}

	if (parsetree->setOperations != NULL)
	{
		/*
		 * There's noplace to put the qual on a setop statement, either. (This
		 * could be fixed, but right now the planner simply ignores any qual
		 * condition on a setop query.)
		 */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("conditional UNION/INTERSECT/EXCEPT statements are not implemented")));
	}

	/* INTERSECT wants the original, but we need to copy - Jan */
	copy = copyObject(qual);

	parsetree->jointree->quals = make_and_qual(parsetree->jointree->quals,
											   copy);

	/*
	 * We had better not have stuck an aggregate into the WHERE clause.
	 */
	Assert(!contain_aggs_of_level(copy, 0));

	/*
	 * Make sure query is marked correctly if added qual has sublinks. Need
	 * not search qual when query is already marked.
	 */
	if (!parsetree->hasSubLinks)
		parsetree->hasSubLinks = checkExprHasSubLink(copy);
}


/*
 * Invert the given clause and add it to the WHERE qualifications of the
 * given querytree.  Inversion means "x IS NOT TRUE", not just "NOT x",
 * else we will do the wrong thing when x evaluates to NULL.
 */
void
AddInvertedQual(Query *parsetree, Node *qual)
{
	BooleanTest *invqual;

	if (qual == NULL)
		return;

	/* Need not copy input qual, because AddQual will... */
	invqual = makeNode(BooleanTest);
	invqual->arg = (Expr *) qual;
	invqual->booltesttype = IS_NOT_TRUE;
	invqual->location = -1;

	AddQual(parsetree, (Node *) invqual);
}


/*
 * replace_rte_variables() finds all Vars in an expression tree
 * that reference a particular RTE, and replaces them with substitute
 * expressions obtained from a caller-supplied callback function.
 *
 * When invoking replace_rte_variables on a portion of a Query, pass the
 * address of the containing Query's hasSubLinks field as outer_hasSubLinks.
 * Otherwise, pass NULL, but inserting a SubLink into a non-Query expression
 * will then cause an error.
 *
 * Note: the business with inserted_sublink is needed to update hasSubLinks
 * in subqueries when the replacement adds a subquery inside a subquery.
 * Messy, isn't it?  We do not need to do similar pushups for hasAggs,
 * because it isn't possible for this transformation to insert a level-zero
 * aggregate reference into a subquery --- it could only insert outer aggs.
 * Likewise for hasWindowFuncs.
 *
 * Note: usually, we'd not expose the mutator function or context struct
 * for a function like this.  We do so because callbacks often find it
 * convenient to recurse directly to the mutator on sub-expressions of
 * what they will return.
 */
Node *
replace_rte_variables(Node *node, int target_varno, int sublevels_up,
					  replace_rte_variables_callback callback,
					  void *callback_arg,
					  bool *outer_hasSubLinks)
{
	Node	   *result;
	replace_rte_variables_context context;

	context.callback = callback;
	context.callback_arg = callback_arg;
	context.target_varno = target_varno;
	context.sublevels_up = sublevels_up;

	/*
	 * We try to initialize inserted_sublink to true if there is no need to
	 * detect new sublinks because the query already has some.
	 */
	if (node && IsA(node, Query))
		context.inserted_sublink = ((Query *) node)->hasSubLinks;
	else if (outer_hasSubLinks)
		context.inserted_sublink = *outer_hasSubLinks;
	else
		context.inserted_sublink = false;

	/*
	 * Must be prepared to start with a Query or a bare expression tree; if
	 * it's a Query, we don't want to increment sublevels_up.
	 */
	result = query_or_expression_tree_mutator(node,
											  replace_rte_variables_mutator,
											  (void *) &context,
											  0);

	if (context.inserted_sublink)
	{
		if (result && IsA(result, Query))
			((Query *) result)->hasSubLinks = true;
		else if (outer_hasSubLinks)
			*outer_hasSubLinks = true;
		else
			elog(ERROR, "replace_rte_variables inserted a SubLink, but has noplace to record it");
	}

	return result;
}

Node *
replace_rte_variables_mutator(Node *node,
							  replace_rte_variables_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == context->target_varno &&
			var->varlevelsup == context->sublevels_up)
		{
			/* Found a matching variable, make the substitution */
			Node	   *newnode;

			newnode = context->callback(var, context);
			/* Detect if we are adding a sublink to query */
			if (!context->inserted_sublink)
				context->inserted_sublink = checkExprHasSubLink(newnode);
			return newnode;
		}
		/* otherwise fall through to copy the var normally */
	}
	else if (IsA(node, CurrentOfExpr))
	{
		CurrentOfExpr *cexpr = (CurrentOfExpr *) node;

		if (cexpr->cvarno == context->target_varno &&
			context->sublevels_up == 0)
		{
			/*
			 * We get here if a WHERE CURRENT OF expression turns out to apply
			 * to a view.  Someday we might be able to translate the
			 * expression to apply to an underlying table of the view, but
			 * right now it's not implemented.
			 */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("WHERE CURRENT OF on a view is not implemented")));
		}
		/* otherwise fall through to copy the expr normally */
	}
	else if (IsA(node, Query))
	{
		/* Recurse into RTE subquery or not-yet-planned sublink subquery */
		Query	   *newnode;
		bool		save_inserted_sublink;

		context->sublevels_up++;
		save_inserted_sublink = context->inserted_sublink;
		context->inserted_sublink = ((Query *) node)->hasSubLinks;
		newnode = query_tree_mutator((Query *) node,
									 replace_rte_variables_mutator,
									 (void *) context,
									 0);
		newnode->hasSubLinks |= context->inserted_sublink;
		context->inserted_sublink = save_inserted_sublink;
		context->sublevels_up--;
		return (Node *) newnode;
	}
	return expression_tree_mutator(node, replace_rte_variables_mutator,
								   (void *) context);
}


/*
 * map_variable_attnos() finds all user-column Vars in an expression tree
 * that reference a particular RTE, and adjusts their varattnos according
 * to the given mapping array (varattno n is replaced by attno_map[n-1]).
 * Vars for system columns are not modified.
 *
 * A zero in the mapping array represents a dropped column, which should not
 * appear in the expression.
 *
 * If the expression tree contains a whole-row Var for the target RTE,
 * *found_whole_row is set to true.  In addition, if to_rowtype is
 * not InvalidOid, we replace the Var with a Var of that vartype, inserting
 * a ConvertRowtypeExpr to map back to the rowtype expected by the expression.
 * (Therefore, to_rowtype had better be a child rowtype of the rowtype of the
 * RTE we're changing references to.)  Callers that don't provide to_rowtype
 * should report an error if *found_row_type is true; we don't do that here
 * because we don't know exactly what wording for the error message would
 * be most appropriate.  The caller will be aware of the context.
 *
 * This could be built using replace_rte_variables and a callback function,
 * but since we don't ever need to insert sublinks, replace_rte_variables is
 * overly complicated.
 */

typedef struct
{
	int			target_varno;	/* RTE index to search for */
	int			sublevels_up;	/* (current) nesting depth */
	const AttrNumber *attno_map;	/* map array for user attnos */
	int			map_length;		/* number of entries in attno_map[] */
	Oid			to_rowtype;		/* change whole-row Vars to this type */
	bool	   *found_whole_row;	/* output flag */
} map_variable_attnos_context;

static Node *
map_variable_attnos_mutator(Node *node,
							map_variable_attnos_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == context->target_varno &&
			var->varlevelsup == context->sublevels_up)
		{
			/* Found a matching variable, make the substitution */
			Var		   *newvar = (Var *) palloc(sizeof(Var));
			int			attno = var->varattno;

			*newvar = *var;		/* initially copy all fields of the Var */

			if (attno > 0)
			{
				/* user-defined column, replace attno */
				if (attno > context->map_length ||
					context->attno_map[attno - 1] == 0)
					elog(ERROR, "unexpected varattno %d in expression to be mapped",
						 attno);
				newvar->varattno = newvar->varoattno = context->attno_map[attno - 1];
			}
			else if (attno == 0)
			{
				/* whole-row variable, warn caller */
				*(context->found_whole_row) = true;

				/* If the caller expects us to convert the Var, do so. */
				if (OidIsValid(context->to_rowtype) &&
					context->to_rowtype != var->vartype)
				{
					ConvertRowtypeExpr *r;

					/* This certainly won't work for a RECORD variable. */
					Assert(var->vartype != RECORDOID);

					/* Var itself is changed to the requested type. */
					newvar->vartype = context->to_rowtype;

					/*
					 * Add a conversion node on top to convert back to the
					 * original type expected by the expression.
					 */
					r = makeNode(ConvertRowtypeExpr);
					r->arg = (Expr *) newvar;
					r->resulttype = var->vartype;
					r->convertformat = COERCE_IMPLICIT_CAST;
					r->location = -1;

					return (Node *) r;
				}
			}
			return (Node *) newvar;
		}
		/* otherwise fall through to copy the var normally */
	}
	else if (IsA(node, ConvertRowtypeExpr))
	{
		ConvertRowtypeExpr *r = (ConvertRowtypeExpr *) node;
		Var		   *var = (Var *) r->arg;

		/*
		 * If this is coercing a whole-row Var that we need to convert, then
		 * just convert the Var without adding an extra ConvertRowtypeExpr.
		 * Effectively we're simplifying var::parenttype::grandparenttype into
		 * just var::grandparenttype.  This avoids building stacks of CREs if
		 * this function is applied repeatedly.
		 */
		if (IsA(var, Var) &&
			var->varno == context->target_varno &&
			var->varlevelsup == context->sublevels_up &&
			var->varattno == 0 &&
			OidIsValid(context->to_rowtype) &&
			context->to_rowtype != var->vartype)
		{
			ConvertRowtypeExpr *newnode;
			Var		   *newvar = (Var *) palloc(sizeof(Var));

			/* whole-row variable, warn caller */
			*(context->found_whole_row) = true;

			*newvar = *var;		/* initially copy all fields of the Var */

			/* This certainly won't work for a RECORD variable. */
			Assert(var->vartype != RECORDOID);

			/* Var itself is changed to the requested type. */
			newvar->vartype = context->to_rowtype;

			newnode = (ConvertRowtypeExpr *) palloc(sizeof(ConvertRowtypeExpr));
			*newnode = *r;		/* initially copy all fields of the CRE */
			newnode->arg = (Expr *) newvar;

			return (Node *) newnode;
		}
		/* otherwise fall through to process the expression normally */
	}
	else if (IsA(node, Query))
	{
		/* Recurse into RTE subquery or not-yet-planned sublink subquery */
		Query	   *newnode;

		context->sublevels_up++;
		newnode = query_tree_mutator((Query *) node,
									 map_variable_attnos_mutator,
									 (void *) context,
									 0);
		context->sublevels_up--;
		return (Node *) newnode;
	}
	return expression_tree_mutator(node, map_variable_attnos_mutator,
								   (void *) context);
}

Node *
map_variable_attnos(Node *node,
					int target_varno, int sublevels_up,
					const AttrNumber *attno_map, int map_length,
					Oid to_rowtype, bool *found_whole_row)
{
	map_variable_attnos_context context;

	context.target_varno = target_varno;
	context.sublevels_up = sublevels_up;
	context.attno_map = attno_map;
	context.map_length = map_length;
	context.to_rowtype = to_rowtype;
	context.found_whole_row = found_whole_row;

	*found_whole_row = false;

	/*
	 * Must be prepared to start with a Query or a bare expression tree; if
	 * it's a Query, we don't want to increment sublevels_up.
	 */
	return query_or_expression_tree_mutator(node,
											map_variable_attnos_mutator,
											(void *) &context,
											0);
}


/*
 * ReplaceVarsFromTargetList - replace Vars with items from a targetlist
 *
 * Vars matching target_varno and sublevels_up are replaced by the
 * entry with matching resno from targetlist, if there is one.
 *
 * If there is no matching resno for such a Var, the action depends on the
 * nomatch_option:
 *	REPLACEVARS_REPORT_ERROR: throw an error
 *	REPLACEVARS_CHANGE_VARNO: change Var's varno to nomatch_varno
 *	REPLACEVARS_SUBSTITUTE_NULL: replace Var with a NULL Const of same type
 *
 * The caller must also provide target_rte, the RTE describing the target
 * relation.  This is needed to handle whole-row Vars referencing the target.
 * We expand such Vars into RowExpr constructs.
 *
 * outer_hasSubLinks works the same as for replace_rte_variables().
 */

typedef struct
{
	RangeTblEntry *target_rte;
	List	   *targetlist;
	ReplaceVarsNoMatchOption nomatch_option;
	int			nomatch_varno;
} ReplaceVarsFromTargetList_context;

static Node *
ReplaceVarsFromTargetList_callback(Var *var,
								   replace_rte_variables_context *context)
{
	ReplaceVarsFromTargetList_context *rcon = (ReplaceVarsFromTargetList_context *) context->callback_arg;
	TargetEntry *tle;

	if (var->varattno == InvalidAttrNumber)
	{
		/* Must expand whole-tuple reference into RowExpr */
		RowExpr    *rowexpr;
		List	   *colnames;
		List	   *fields;

		/*
		 * If generating an expansion for a var of a named rowtype (ie, this
		 * is a plain relation RTE), then we must include dummy items for
		 * dropped columns.  If the var is RECORD (ie, this is a JOIN), then
		 * omit dropped columns.  Either way, attach column names to the
		 * RowExpr for use of ruleutils.c.
		 */
		expandRTE(rcon->target_rte,
				  var->varno, var->varlevelsup, var->location,
				  (var->vartype != RECORDOID),
				  &colnames, &fields);
		/* Adjust the generated per-field Vars... */
		fields = (List *) replace_rte_variables_mutator((Node *) fields,
														context);
		rowexpr = makeNode(RowExpr);
		rowexpr->args = fields;
		rowexpr->row_typeid = var->vartype;
		rowexpr->row_format = COERCE_IMPLICIT_CAST;
		rowexpr->colnames = colnames;
		rowexpr->location = var->location;

		return (Node *) rowexpr;
	}

	/* Normal case referencing one targetlist element */
	tle = get_tle_by_resno(rcon->targetlist, var->varattno);

	if (tle == NULL || tle->resjunk)
	{
		/* Failed to find column in targetlist */
		switch (rcon->nomatch_option)
		{
			case REPLACEVARS_REPORT_ERROR:
				/* fall through, throw error below */
				break;

			case REPLACEVARS_CHANGE_VARNO:
				var = (Var *) copyObject(var);
				var->varno = rcon->nomatch_varno;
				var->varnoold = rcon->nomatch_varno;
				return (Node *) var;

			case REPLACEVARS_SUBSTITUTE_NULL:

				/*
				 * If Var is of domain type, we should add a CoerceToDomain
				 * node, in case there is a NOT NULL domain constraint.
				 */
				return coerce_to_domain((Node *) makeNullConst(var->vartype,
															   var->vartypmod,
															   var->varcollid),
										InvalidOid, -1,
										var->vartype,
										COERCION_IMPLICIT,
										COERCE_IMPLICIT_CAST,
										-1,
										false);
		}
		elog(ERROR, "could not find replacement targetlist entry for attno %d",
			 var->varattno);
		return NULL;			/* keep compiler quiet */
	}
	else
	{
		/* Make a copy of the tlist item to return */
		Expr	   *newnode = copyObject(tle->expr);

		/* Must adjust varlevelsup if tlist item is from higher query */
		if (var->varlevelsup > 0)
			IncrementVarSublevelsUp((Node *) newnode, var->varlevelsup, 0);

		/*
		 * Check to see if the tlist item contains a PARAM_MULTIEXPR Param,
		 * and throw error if so.  This case could only happen when expanding
		 * an ON UPDATE rule's NEW variable and the referenced tlist item in
		 * the original UPDATE command is part of a multiple assignment. There
		 * seems no practical way to handle such cases without multiple
		 * evaluation of the multiple assignment's sub-select, which would
		 * create semantic oddities that users of rules would probably prefer
		 * not to cope with.  So treat it as an unimplemented feature.
		 */
		if (contains_multiexpr_param((Node *) newnode, NULL))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("NEW variables in ON UPDATE rules cannot reference columns that are part of a multiple assignment in the subject UPDATE command")));

		return (Node *) newnode;
	}
}

Node *
ReplaceVarsFromTargetList(Node *node,
						  int target_varno, int sublevels_up,
						  RangeTblEntry *target_rte,
						  List *targetlist,
						  ReplaceVarsNoMatchOption nomatch_option,
						  int nomatch_varno,
						  bool *outer_hasSubLinks)
{
	ReplaceVarsFromTargetList_context context;

	context.target_rte = target_rte;
	context.targetlist = targetlist;
	context.nomatch_option = nomatch_option;
	context.nomatch_varno = nomatch_varno;

	return replace_rte_variables(node, target_varno, sublevels_up,
								 ReplaceVarsFromTargetList_callback,
								 (void *) &context,
								 outer_hasSubLinks);
}
