/*-------------------------------------------------------------------------
 *
 * rewriteManip.c
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteManip.c,v 1.80.2.1 2006/01/06 20:11:31 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"


typedef struct
{
	int			sublevels_up;
} checkExprHasAggs_context;

static bool checkExprHasAggs_walker(Node *node,
						checkExprHasAggs_context *context);
static bool checkExprHasSubLink_walker(Node *node, void *context);
static Relids offset_relid_set(Relids relids, int offset);
static Relids adjust_relid_set(Relids relids, int oldrelid, int newrelid);


/*
 * checkExprHasAggs -
 *	Queries marked hasAggs might not have them any longer after
 *	rewriting. Check it.
 *
 * The objective of this routine is to detect whether there are aggregates
 * belonging to the initial query level.  Aggregates belonging to subqueries
 * or outer queries do NOT cause a true result.  We must recurse into
 * subqueries to detect outer-reference aggregates that logically belong to
 * the initial query level.
 */
bool
checkExprHasAggs(Node *node)
{
	checkExprHasAggs_context context;

	context.sublevels_up = 0;

	/*
	 * Must be prepared to start with a Query or a bare expression tree;
	 * if it's a Query, we don't want to increment sublevels_up.
	 */
	return query_or_expression_tree_walker(node,
										   checkExprHasAggs_walker,
										   (void *) &context,
										   0);
}

static bool
checkExprHasAggs_walker(Node *node, checkExprHasAggs_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		if (((Aggref *) node)->agglevelsup == context->sublevels_up)
			return true;		/* abort the tree traversal and return
								 * true */
		/* else fall through to examine argument */
	}
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   checkExprHasAggs_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, checkExprHasAggs_walker,
								  (void *) context);
}

/*
 * checkExprHasSubLink -
 *	Queries marked hasSubLinks might not have them any longer after
 *	rewriting. Check it.
 */
bool
checkExprHasSubLink(Node *node)
{
	/*
	 * If a Query is passed, examine it --- but we need not recurse into
	 * sub-Queries.
	 */
	return query_or_expression_tree_walker(node,
										   checkExprHasSubLink_walker,
										   NULL,
										   QTW_IGNORE_RT_SUBQUERIES);
}

static bool
checkExprHasSubLink_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
		return true;			/* abort the tree traversal and return
								 * true */
	return expression_tree_walker(node, checkExprHasSubLink_walker, context);
}


/*
 * OffsetVarNodes - adjust Vars when appending one query's RT to another
 *
 * Find all Var nodes in the given tree with varlevelsup == sublevels_up,
 * and increment their varno fields (rangetable indexes) by 'offset'.
 * The varnoold fields are adjusted similarly.	Also, adjust other nodes
 * that contain rangetable indexes, such as RangeTblRef and JoinExpr.
 *
 * NOTE: although this has the form of a walker, we cheat and modify the
 * nodes in-place.	The given expression tree should have been copied
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

		if (context->sublevels_up == 0)
			j->rtindex += context->offset;
		/* fall through to examine children */
	}
	if (IsA(node, InClauseInfo))
	{
		InClauseInfo *ininfo = (InClauseInfo *) node;

		if (context->sublevels_up == 0)
		{
			ininfo->lefthand = offset_relid_set(ininfo->lefthand,
												context->offset);
			ininfo->righthand = offset_relid_set(ininfo->righthand,
												 context->offset);
		}
		/* fall through to examine children */
	}
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
	 * Must be prepared to start with a Query or a bare expression tree;
	 * if it's a Query, go straight to query_tree_walker to make sure that
	 * sublevels_up doesn't get incremented prematurely.
	 */
	if (node && IsA(node, Query))
	{
		Query	   *qry = (Query *) node;
		List	   *l;

		/*
		 * If we are starting at a Query, and sublevels_up is zero, then
		 * we must also fix rangetable indexes in the Query itself ---
		 * namely resultRelation and rowMarks entries.	sublevels_up
		 * cannot be zero when recursing into a subquery, so there's no
		 * need to have the same logic inside OffsetVarNodes_walker.
		 */
		if (sublevels_up == 0)
		{
			if (qry->resultRelation)
				qry->resultRelation += offset;
			foreach(l, qry->rowMarks)
				lfirsti(l) += offset;
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
	Relids		tmprelids;
	int			rtindex;

	tmprelids = bms_copy(relids);
	while ((rtindex = bms_first_member(tmprelids)) >= 0)
		result = bms_add_member(result, rtindex + offset);
	bms_free(tmprelids);
	return result;
}

/*
 * ChangeVarNodes - adjust Var nodes for a specific change of RT index
 *
 * Find all Var nodes in the given tree belonging to a specific relation
 * (identified by sublevels_up and rt_index), and change their varno fields
 * to 'new_index'.	The varnoold fields are changed too.  Also, adjust other
 * nodes that contain rangetable indexes, such as RangeTblRef and JoinExpr.
 *
 * NOTE: although this has the form of a walker, we cheat and modify the
 * nodes in-place.	The given expression tree should have been copied
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
	if (IsA(node, InClauseInfo))
	{
		InClauseInfo *ininfo = (InClauseInfo *) node;

		if (context->sublevels_up == 0)
		{
			ininfo->lefthand = adjust_relid_set(ininfo->lefthand,
												context->rt_index,
												context->new_index);
			ininfo->righthand = adjust_relid_set(ininfo->righthand,
												 context->rt_index,
												 context->new_index);
		}
		/* fall through to examine children */
	}
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
	 * Must be prepared to start with a Query or a bare expression tree;
	 * if it's a Query, go straight to query_tree_walker to make sure that
	 * sublevels_up doesn't get incremented prematurely.
	 */
	if (node && IsA(node, Query))
	{
		Query	   *qry = (Query *) node;
		List	   *l;

		/*
		 * If we are starting at a Query, and sublevels_up is zero, then
		 * we must also fix rangetable indexes in the Query itself ---
		 * namely resultRelation and rowMarks entries.	sublevels_up
		 * cannot be zero when recursing into a subquery, so there's no
		 * need to have the same logic inside ChangeVarNodes_walker.
		 */
		if (sublevels_up == 0)
		{
			if (qry->resultRelation == rt_index)
				qry->resultRelation = new_index;
			foreach(l, qry->rowMarks)
			{
				if (lfirsti(l) == rt_index)
					lfirsti(l) = new_index;
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
 * Aggref nodes are adjusted similarly.
 *
 * NOTE: although this has the form of a walker, we cheat and modify the
 * Var nodes in-place.	The given expression tree should have been copied
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
	if (IsA(node, Aggref))
	{
		Aggref	   *agg = (Aggref *) node;

		if (agg->agglevelsup >= context->min_sublevels_up)
			agg->agglevelsup += context->delta_sublevels_up;
		/* fall through to recurse into argument */
	}
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->min_sublevels_up++;
		result = query_tree_walker((Query *) node,
								   IncrementVarSublevelsUp_walker,
								   (void *) context, 0);
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
	 * Must be prepared to start with a Query or a bare expression tree;
	 * if it's a Query, we don't want to increment sublevels_up.
	 */
	query_or_expression_tree_walker(node,
									IncrementVarSublevelsUp_walker,
									(void *) &context,
									0);
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
	if (IsA(node, InClauseInfo))
	{
		InClauseInfo *ininfo = (InClauseInfo *) node;

		if (context->sublevels_up == 0 &&
			(bms_is_member(context->rt_index, ininfo->lefthand) ||
			 bms_is_member(context->rt_index, ininfo->righthand)))
			return true;
		/* fall through to examine children */
	}
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
	 * Must be prepared to start with a Query or a bare expression tree;
	 * if it's a Query, we don't want to increment sublevels_up.
	 */
	return query_or_expression_tree_walker(node,
										   rangeTableEntry_used_walker,
										   (void *) &context,
										   0);
}


/*
 * attribute_used -
 *	Check if a specific attribute number of a RTE is used
 *	somewhere in the query or expression.
 */

typedef struct
{
	int			rt_index;
	int			attno;
	int			sublevels_up;
} attribute_used_context;

static bool
attribute_used_walker(Node *node,
					  attribute_used_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up &&
			var->varno == context->rt_index &&
			var->varattno == context->attno)
			return true;
		return false;
	}
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node, attribute_used_walker,
								   (void *) context, 0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, attribute_used_walker,
								  (void *) context);
}

bool
attribute_used(Node *node, int rt_index, int attno, int sublevels_up)
{
	attribute_used_context context;

	context.rt_index = rt_index;
	context.attno = attno;
	context.sublevels_up = sublevels_up;

	/*
	 * Must be prepared to start with a Query or a bare expression tree;
	 * if it's a Query, we don't want to increment sublevels_up.
	 */
	return query_or_expression_tree_walker(node,
										   attribute_used_walker,
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
	 * expect to find the *OLD* and *NEW* placeholder entries in the given
	 * query.  If they're not there, it must be an INSERT/SELECT in which
	 * they've been pushed down to the SELECT.
	 */
	if (length(parsetree->rtable) >= 2 &&
	 strcmp(rt_fetch(PRS2_OLD_VARNO, parsetree->rtable)->eref->aliasname,
			"*OLD*") == 0 &&
	 strcmp(rt_fetch(PRS2_NEW_VARNO, parsetree->rtable)->eref->aliasname,
			"*NEW*") == 0)
		return parsetree;
	Assert(parsetree->jointree && IsA(parsetree->jointree, FromExpr));
	if (length(parsetree->jointree->fromlist) != 1)
		elog(ERROR, "expected to find SELECT subquery");
	rtr = (RangeTblRef *) lfirst(parsetree->jointree->fromlist);
	Assert(IsA(rtr, RangeTblRef));
	selectrte = rt_fetch(rtr->rtindex, parsetree->rtable);
	selectquery = selectrte->subquery;
	if (!(selectquery && IsA(selectquery, Query) &&
		  selectquery->commandType == CMD_SELECT))
		elog(ERROR, "expected to find SELECT subquery");
	if (length(selectquery->rtable) >= 2 &&
	strcmp(rt_fetch(PRS2_OLD_VARNO, selectquery->rtable)->eref->aliasname,
		   "*OLD*") == 0 &&
	strcmp(rt_fetch(PRS2_NEW_VARNO, selectquery->rtable)->eref->aliasname,
		   "*NEW*") == 0)
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
		 * NOTIFY will execute, whether or not there are any qualifying
		 * rows. While clearly wrong, this is much more useful than
		 * refusing to execute the rule at all, and extra NOTIFY events
		 * are harmless for typical uses of NOTIFY.
		 *
		 * If it isn't a NOTIFY, error out, since unconditional execution of
		 * other utility stmts is unlikely to be wanted.  (This case is
		 * not currently allowed anyway, but keep the test for safety.)
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
		 * There's noplace to put the qual on a setop statement, either.
		 * (This could be fixed, but right now the planner simply ignores
		 * any qual condition on a setop query.)
		 */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("conditional UNION/INTERSECT/EXCEPT statements are not implemented")));
	}

	/* INTERSECT want's the original, but we need to copy - Jan */
	copy = copyObject(qual);

	parsetree->jointree->quals = make_and_qual(parsetree->jointree->quals,
											   copy);

	/*
	 * Make sure query is marked correctly if added qual has sublinks or
	 * aggregates (not sure it can ever have aggs, but sublinks
	 * definitely).  Need not search qual when query is already marked.
	 */
	if (!parsetree->hasAggs)
		parsetree->hasAggs = checkExprHasAggs(copy);
	if (!parsetree->hasSubLinks)
		parsetree->hasSubLinks = checkExprHasSubLink(copy);
}

/*
 * Add the given havingQual to the one already contained in the parsetree
 * just as AddQual does for the normal 'where' qual
 */
void
AddHavingQual(Query *parsetree, Node *havingQual)
{
	Node	   *copy;

	if (havingQual == NULL)
		return;

	if (parsetree->commandType == CMD_UTILITY)
	{
		/*
		 * There's noplace to put the qual on a utility statement.
		 *
		 * See comments in AddQual for motivation.
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
		 * There's noplace to put the qual on a setop statement, either.
		 * (This could be fixed, but right now the planner simply ignores
		 * any qual condition on a setop query.)
		 */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("conditional UNION/INTERSECT/EXCEPT statements are not implemented")));
	}

	/* INTERSECT want's the original, but we need to copy - Jan */
	copy = copyObject(havingQual);

	parsetree->havingQual = make_and_qual(parsetree->havingQual,
										  copy);

	/*
	 * Make sure query is marked correctly if added qual has sublinks or
	 * aggregates (not sure it can ever have aggs, but sublinks
	 * definitely).  Need not search qual when query is already marked.
	 */
	if (!parsetree->hasAggs)
		parsetree->hasAggs = checkExprHasAggs(copy);
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

	AddQual(parsetree, (Node *) invqual);
}


/*
 * ResolveNew - replace Vars with corresponding items from a targetlist
 *
 * Vars matching target_varno and sublevels_up are replaced by the
 * entry with matching resno from targetlist, if there is one.
 * If not, we either change the unmatched Var's varno to update_varno
 * (when event == CMD_UPDATE) or replace it with a constant NULL.
 *
 * Note: the business with inserted_sublink is needed to update hasSubLinks
 * in subqueries when the replacement adds a subquery inside a subquery.
 * Messy, isn't it?  We do not need to do similar pushups for hasAggs,
 * because it isn't possible for this transformation to insert a level-zero
 * aggregate reference into a subquery --- it could only insert outer aggs.
 */

typedef struct
{
	int			target_varno;
	int			sublevels_up;
	List	   *targetlist;
	int			event;
	int			update_varno;
	bool		inserted_sublink;
} ResolveNew_context;

static Node *
ResolveNew_mutator(Node *node, ResolveNew_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		int			this_varno = (int) var->varno;
		int			this_varlevelsup = (int) var->varlevelsup;

		if (this_varno == context->target_varno &&
			this_varlevelsup == context->sublevels_up)
		{
			TargetEntry *tle;

			/* band-aid: don't do the wrong thing with a whole-tuple Var */
			if (var->varattno == InvalidAttrNumber)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot handle whole-row reference")));

			tle = get_tle_by_resno(context->targetlist, var->varattno);

			if (tle == NULL)
			{
				if (context->event == CMD_UPDATE)
				{
					/* For update, just change unmatched var's varno */
					var = (Var *) copyObject(node);
					var->varno = context->update_varno;
					var->varnoold = context->update_varno;
					return (Node *) var;
				}
				else
				{
					/* Otherwise replace unmatched var with a null */
					/* need coerce_to_domain in case of NOT NULL domain constraint */
					return coerce_to_domain((Node *) makeNullConst(var->vartype),
											InvalidOid,
											var->vartype,
											COERCE_IMPLICIT_CAST);
				}
			}
			else
			{
				/* Make a copy of the tlist item to return */
				Node	   *n = copyObject(tle->expr);

				/* Adjust varlevelsup if tlist item is from higher query */
				if (this_varlevelsup > 0)
					IncrementVarSublevelsUp(n, this_varlevelsup, 0);
				/* Report it if we are adding a sublink to query */
				if (!context->inserted_sublink)
					context->inserted_sublink = checkExprHasSubLink(n);
				return n;
			}
		}
		/* otherwise fall through to copy the var normally */
	}

	if (IsA(node, Query))
	{
		/* Recurse into RTE subquery or not-yet-planned sublink subquery */
		Query	   *newnode;
		bool		save_inserted_sublink;

		context->sublevels_up++;
		save_inserted_sublink = context->inserted_sublink;
		context->inserted_sublink = false;
		newnode = query_tree_mutator((Query *) node,
									 ResolveNew_mutator,
									 (void *) context,
									 0);
		newnode->hasSubLinks |= context->inserted_sublink;
		context->inserted_sublink = save_inserted_sublink;
		context->sublevels_up--;
		return (Node *) newnode;
	}
	return expression_tree_mutator(node, ResolveNew_mutator,
								   (void *) context);
}

Node *
ResolveNew(Node *node, int target_varno, int sublevels_up,
		   List *targetlist, int event, int update_varno)
{
	ResolveNew_context context;

	context.target_varno = target_varno;
	context.sublevels_up = sublevels_up;
	context.targetlist = targetlist;
	context.event = event;
	context.update_varno = update_varno;
	context.inserted_sublink = false;

	/*
	 * Must be prepared to start with a Query or a bare expression tree;
	 * if it's a Query, we don't want to increment sublevels_up.
	 */
	return query_or_expression_tree_mutator(node,
											ResolveNew_mutator,
											(void *) &context,
											0);
}
