/*-------------------------------------------------------------------------
 *
 * rewriteHandler.c
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteHandler.c,v 1.70 2000/04/04 02:30:52 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/prep.h"
#include "optimizer/var.h"
#include "parser/analyze.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_oper.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "parser/parsetree.h"
#include "rewrite/locks.h"
#include "rewrite/rewriteManip.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"


extern void CheckSelectForUpdate(Query *rule_action);	/* in analyze.c */


/* macros borrowed from expression_tree_mutator */

#define FLATCOPY(newnode, node, nodetype)  \
	( (newnode) = makeNode(nodetype), \
	  memcpy((newnode), (node), sizeof(nodetype)) )

#define MUTATE(newfield, oldfield, fieldtype, mutator, context)  \
		( (newfield) = (fieldtype) mutator((Node *) (oldfield), (context)) )


static RewriteInfo *gatherRewriteMeta(Query *parsetree,
				  Query *rule_action,
				  Node *rule_qual,
				  int rt_index,
				  CmdType event,
				  bool *instead_flag);
static bool rangeTableEntry_used(Node *node, int rt_index, int sublevels_up);
static bool attribute_used(Node *node, int rt_index, int attno,
						   int sublevels_up);
static bool modifyAggrefChangeVarnodes(Node *node, int rt_index, int new_index,
									   int sublevels_up, int new_sublevels_up);
static Node *modifyAggrefDropQual(Node *node, Node *targetNode);
static SubLink *modifyAggrefMakeSublink(Aggref *aggref, Query *parsetree);
static Node *modifyAggrefQual(Node *node, Query *parsetree);
static Query *fireRIRrules(Query *parsetree);
static Query *Except_Intersect_Rewrite(Query *parsetree);
static void check_targetlists_are_compatible(List *prev_target,
											 List *current_target);
static void create_intersect_list(Node *ptr, List **intersect_list);
static Node *intersect_tree_analyze(Node *tree, Node *first_select,
									Node *parsetree);

/*
 * gatherRewriteMeta -
 *	  Gather meta information about parsetree, and rule. Fix rule body
 *	  and qualifier so that they can be mixed with the parsetree and
 *	  maintain semantic validity
 */
static RewriteInfo *
gatherRewriteMeta(Query *parsetree,
				  Query *rule_action,
				  Node *rule_qual,
				  int rt_index,
				  CmdType event,
				  bool *instead_flag)
{
	RewriteInfo *info;
	int			rt_length;
	int			result_reln;

	info = (RewriteInfo *) palloc(sizeof(RewriteInfo));
	info->rt_index = rt_index;
	info->event = event;
	info->instead_flag = *instead_flag;
	info->rule_action = (Query *) copyObject(rule_action);
	info->rule_qual = (Node *) copyObject(rule_qual);
	if (info->rule_action == NULL)
		info->nothing = TRUE;
	else
	{
		info->nothing = FALSE;
		info->action = info->rule_action->commandType;
		info->current_varno = rt_index;
		info->rt = parsetree->rtable;
		rt_length = length(info->rt);
		info->rt = nconc(info->rt, copyObject(info->rule_action->rtable));

		info->new_varno = PRS2_NEW_VARNO + rt_length;
		OffsetVarNodes(info->rule_action->qual, rt_length, 0);
		OffsetVarNodes((Node *) info->rule_action->targetList, rt_length, 0);
		OffsetVarNodes(info->rule_qual, rt_length, 0);
		ChangeVarNodes((Node *) info->rule_action->qual,
					   PRS2_CURRENT_VARNO + rt_length, rt_index, 0);
		ChangeVarNodes((Node *) info->rule_action->targetList,
					   PRS2_CURRENT_VARNO + rt_length, rt_index, 0);
		ChangeVarNodes(info->rule_qual,
					   PRS2_CURRENT_VARNO + rt_length, rt_index, 0);

		/*
		 * bug here about replace CURRENT  -- sort of replace current is
		 * deprecated now so this code shouldn't really need to be so
		 * clutzy but.....
		 */
		if (info->action != CMD_SELECT)
		{						/* i.e update XXXXX */
			int			new_result_reln = 0;

			result_reln = info->rule_action->resultRelation;
			switch (result_reln)
			{
				case PRS2_CURRENT_VARNO:
					new_result_reln = rt_index;
					break;
				case PRS2_NEW_VARNO:	/* XXX */
				default:
					new_result_reln = result_reln + rt_length;
					break;
			}
			info->rule_action->resultRelation = new_result_reln;
		}
	}
	return info;
}


/*
 * rangeTableEntry_used -
 *	we need to process a RTE for RIR rules only if it is
 *	referenced somewhere in var nodes of the query.
 */

typedef struct {
	int			rt_index;
	int			sublevels_up;
} rangeTableEntry_used_context;

static bool
rangeTableEntry_used_walker (Node *node,
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
	if (IsA(node, SubLink))
	{
		/*
		 * Standard expression_tree_walker will not recurse into subselect,
		 * but here we must do so.
		 */
		SubLink    *sub = (SubLink *) node;

		if (rangeTableEntry_used_walker((Node *) (sub->lefthand), context))
			return true;
		if (rangeTableEntry_used((Node *) (sub->subselect),
								 context->rt_index,
								 context->sublevels_up + 1))
			return true;
		return false;
	}
	if (IsA(node, Query))
	{
		/* Reach here after recursing down into subselect above... */
		Query	   *qry = (Query *) node;

		if (rangeTableEntry_used_walker((Node *) (qry->targetList), context))
			return true;
		if (rangeTableEntry_used_walker((Node *) (qry->qual), context))
			return true;
		if (rangeTableEntry_used_walker((Node *) (qry->havingQual), context))
			return true;
		return false;
	}
	return expression_tree_walker(node, rangeTableEntry_used_walker,
								  (void *) context);
}

static bool
rangeTableEntry_used(Node *node, int rt_index, int sublevels_up)
{
	rangeTableEntry_used_context context;

	context.rt_index = rt_index;
	context.sublevels_up = sublevels_up;
	return rangeTableEntry_used_walker(node, &context);
}


/*
 * attribute_used -
 *	Check if a specific attribute number of a RTE is used
 *	somewhere in the query
 */

typedef struct {
	int			rt_index;
	int			attno;
	int			sublevels_up;
} attribute_used_context;

static bool
attribute_used_walker (Node *node,
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
	if (IsA(node, SubLink))
	{
		/*
		 * Standard expression_tree_walker will not recurse into subselect,
		 * but here we must do so.
		 */
		SubLink    *sub = (SubLink *) node;

		if (attribute_used_walker((Node *) (sub->lefthand), context))
			return true;
		if (attribute_used((Node *) (sub->subselect),
						   context->rt_index,
						   context->attno,
						   context->sublevels_up + 1))
			return true;
		return false;
	}
	if (IsA(node, Query))
	{
		/* Reach here after recursing down into subselect above... */
		Query	   *qry = (Query *) node;

		if (attribute_used_walker((Node *) (qry->targetList), context))
			return true;
		if (attribute_used_walker((Node *) (qry->qual), context))
			return true;
		if (attribute_used_walker((Node *) (qry->havingQual), context))
			return true;
		return false;
	}
	return expression_tree_walker(node, attribute_used_walker,
								  (void *) context);
}

static bool
attribute_used(Node *node, int rt_index, int attno, int sublevels_up)
{
	attribute_used_context context;

	context.rt_index = rt_index;
	context.attno = attno;
	context.sublevels_up = sublevels_up;
	return attribute_used_walker(node, &context);
}


/*
 * modifyAggrefChangeVarnodes -
 *	Change the var nodes in a sublink created for an aggregate column
 *	used in the qualification to point to the correct local RTE.
 *
 * XXX if we still need this after redoing querytree design, it should
 * be combined with ChangeVarNodes, which is the same thing except for
 * not having the option to adjust the vars' varlevelsup.
 *
 * NOTE: although this has the form of a walker, we cheat and modify the
 * Var nodes in-place.  The given expression tree should have been copied
 * earlier to ensure that no unwanted side-effects occur!
 */

typedef struct {
	int			rt_index;
	int			new_index;
	int			sublevels_up;
	int			new_sublevels_up;
} modifyAggrefChangeVarnodes_context;

static bool
modifyAggrefChangeVarnodes_walker(Node *node,
								  modifyAggrefChangeVarnodes_context *context)
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
			var->varlevelsup = context->new_sublevels_up;
		}
		return false;
	}
	if (IsA(node, SubLink))
	{
		/*
		 * Standard expression_tree_walker will not recurse into subselect,
		 * but here we must do so.
		 */
		SubLink    *sub = (SubLink *) node;

		if (modifyAggrefChangeVarnodes_walker((Node *) (sub->lefthand),
											  context))
			return true;
		if (modifyAggrefChangeVarnodes((Node *) (sub->subselect),
									   context->rt_index,
									   context->new_index,
									   context->sublevels_up + 1,
									   context->new_sublevels_up + 1))
			return true;
		return false;
	}
	if (IsA(node, Query))
	{
		/* Reach here after recursing down into subselect above... */
		Query	   *qry = (Query *) node;

		if (modifyAggrefChangeVarnodes_walker((Node *) (qry->targetList),
											  context))
			return true;
		if (modifyAggrefChangeVarnodes_walker((Node *) (qry->qual),
											  context))
			return true;
		if (modifyAggrefChangeVarnodes_walker((Node *) (qry->havingQual),
											  context))
			return true;
		return false;
	}
	return expression_tree_walker(node, modifyAggrefChangeVarnodes_walker,
								  (void *) context);
}

static bool
modifyAggrefChangeVarnodes(Node *node, int rt_index, int new_index,
						   int sublevels_up, int new_sublevels_up)
{
	modifyAggrefChangeVarnodes_context context;

	context.rt_index = rt_index;
	context.new_index = new_index;
	context.sublevels_up = sublevels_up;
	context.new_sublevels_up = new_sublevels_up;
	return modifyAggrefChangeVarnodes_walker(node, &context);
}


/*
 * modifyAggrefDropQual -
 *	remove the pure aggref clause from a qualification
 *
 * targetNode is an Aggref node somewhere within the given expression tree.
 * Find the boolean operator that's presumably somewhere above it, and replace
 * that whole operator expression with a constant TRUE.  (This is NOT really
 * quite the right thing, but it handles simple cases.  This whole set of
 * Aggref-in-qual routines needs to be thrown away when we can do subselects
 * in FROM.)
 *
 * The return tree is a modified copy of the given tree; the given tree
 * is not altered.
 *
 * Note: we don't recurse into subselects looking for targetNode; that's
 * not necessary in the current usage, since in fact targetNode will be
 * within the same select level as the given toplevel node.
 */
static Node *
modifyAggrefDropQual(Node *node, Node *targetNode)
{
	if (node == NULL)
		return NULL;
	if (node == targetNode)
	{
		/* Oops, it's not inside an Expr we can rearrange... */
		elog(ERROR, "Cannot handle aggregate function inserted at this place in WHERE clause");
	}
	if (IsA(node, Expr))
	{
		Expr	   *expr = (Expr *) node;
		List	   *i;

		foreach(i, expr->args)
		{
			if (((Node *) lfirst(i)) == targetNode)
			{
				/* Found the parent expression containing the Aggref */
				if (expr->typeOid != BOOLOID)
					elog(ERROR,
						 "aggregate function in qual must be argument of boolean operator");
				return (Node *) makeConst(BOOLOID, 1, (Datum) true,
										  false, true, false, false);
			}
		}
		/* else this isn't the expr we want, keep going */
	}
	return expression_tree_mutator(node, modifyAggrefDropQual,
								   (void *) targetNode);
}

/*
 * modifyAggrefMakeSublink -
 *	Create a sublink node for a qualification expression that
 *	uses an aggregate column of a view
 */
static SubLink *
modifyAggrefMakeSublink(Aggref *aggref, Query *parsetree)
{
	List	   *aggVarNos;
	/* rte points to old structure: */
	RangeTblEntry *rte;
	/* these point to newly-created structures: */
	Query	   *subquery;
	SubLink    *sublink;
	TargetEntry *tle;
	Resdom	   *resdom;

	aggVarNos = pull_varnos(aggref->target);
	if (length(aggVarNos) != 1)
		elog(ERROR, "rewrite: aggregates of views only allowed on single tables for now");
	rte = rt_fetch(lfirsti(aggVarNos), parsetree->rtable);

	resdom = makeNode(Resdom);
	resdom->resno = 1;
	resdom->restype = aggref->aggtype;
	resdom->restypmod = -1;
	resdom->resname = pstrdup("<noname>");
	resdom->reskey = 0;
	resdom->reskeyop = 0;
	resdom->resjunk = false;

	tle = makeNode(TargetEntry);
	tle->resdom = resdom;
	tle->expr = copyObject(aggref);	/* make a modifiable copy! */

	subquery = makeNode(Query);

	sublink = makeNode(SubLink);
	sublink->subLinkType = EXPR_SUBLINK;
	sublink->useor = false;
	sublink->lefthand = NIL;
	sublink->oper = NIL;
	sublink->subselect = (Node *) subquery;

	subquery->commandType = CMD_SELECT;
	subquery->utilityStmt = NULL;
	subquery->resultRelation = 0;
	subquery->into = NULL;
	subquery->isPortal = FALSE;
	subquery->isBinary = FALSE;
	subquery->isTemp = FALSE;
	subquery->unionall = FALSE;
	subquery->distinctClause = NIL;
	subquery->sortClause = NIL;
	subquery->rtable = lcons(copyObject(rte), NIL);
	subquery->targetList = lcons(tle, NIL);
	subquery->qual = modifyAggrefDropQual((Node *) parsetree->qual,
										  (Node *) aggref);
	/*
	 * If there are still aggs in the subselect's qual, give up.
	 * Recursing would be a bad idea --- we'd likely produce an
	 * infinite recursion.  This whole technique is a crock, really...
	 */
	if (checkExprHasAggs(subquery->qual))
		elog(ERROR, "Cannot handle multiple aggregate functions in WHERE clause");
	subquery->groupClause = NIL;
	subquery->havingQual = NULL;
	subquery->hasAggs = TRUE;
	subquery->hasSubLinks = checkExprHasSubLink(subquery->qual);
	subquery->unionClause = NULL;

	/* Increment all varlevelsup fields in the new subquery */
	IncrementVarSublevelsUp((Node *) subquery, 1, 0);

	/* Replace references to the target table with correct local varno, 1.
	 * Note that because of previous line, these references have
	 * varlevelsup = 1, which must be changed to 0.
	 */
	modifyAggrefChangeVarnodes((Node *) subquery,
							   lfirsti(aggVarNos), 1,
							   1, 0);

	return sublink;
}


/*
 * modifyAggrefQual -
 *	Search for qualification expressions that contain aggregate
 *	functions and substitute them by sublinks. These expressions
 *	originally come from qualifications that use aggregate columns
 *	of a view.
 *
 *	The return value is a modified copy of the given expression tree.
 */
static Node *
modifyAggrefQual(Node *node, Query *parsetree)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Aggref))
	{
		SubLink    *sub = modifyAggrefMakeSublink((Aggref *) node, parsetree);

		parsetree->hasSubLinks = true;
		return (Node *) sub;
	}
	/*
	 * Otherwise, fall through and copy the expr normally.
	 *
	 * We do NOT recurse into subselects in this routine.  It's sufficient
	 * to get rid of aggregates that are in the qual expression proper.
	 */
	return expression_tree_mutator(node, modifyAggrefQual,
								   (void *) parsetree);
}


static Node *
FindMatchingTLEntry(List *tlist, char *e_attname)
{
	List	   *i;

	foreach(i, tlist)
	{
		TargetEntry *tle = lfirst(i);
		char	   *resname;

		resname = tle->resdom->resname;
		if (!strcmp(e_attname, resname))
			return (tle->expr);
	}
	return NULL;
}


static Node *
make_null(Oid type)
{
	Const	   *c = makeNode(Const);

	c->consttype = type;
	c->constlen = get_typlen(type);
	c->constvalue = PointerGetDatum(NULL);
	c->constisnull = true;
	c->constbyval = get_typbyval(type);
	return (Node *) c;
}


/*
 * apply_RIR_view
 *	Replace Vars matching a given RT index with copies of TL expressions.
 */

typedef struct {
	int				rt_index;
	int				sublevels_up;
	RangeTblEntry  *rte;
	List		   *tlist;
	int			   *modified;
} apply_RIR_view_context;

static Node *
apply_RIR_view_mutator(Node *node,
					   apply_RIR_view_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up &&
			var->varno == context->rt_index)
		{
			Node	   *expr;

			if (var->varattno < 0)
				elog(ERROR, "system column %s not available - %s is a view",
					 get_attname(context->rte->relid, var->varattno),
					 context->rte->relname);

			expr = FindMatchingTLEntry(context->tlist,
									   get_attname(context->rte->relid,
												   var->varattno));
			if (expr == NULL)
			{
				/* XXX shouldn't this be an error condition? */
				return make_null(var->vartype);
			}

			/* Make a copy of the tlist item to return */
			expr = copyObject(expr);
			/* Adjust varlevelsup if tlist item is from higher query level */
			if (var->varlevelsup > 0)
				IncrementVarSublevelsUp(expr, var->varlevelsup, 0);

			*(context->modified) = true;
			return (Node *) expr;
		}
		/* otherwise fall through to copy the var normally */
	}
	/*
	 * Since expression_tree_mutator won't touch subselects, we have to
	 * handle them specially.
	 */
	if (IsA(node, SubLink))
	{
		SubLink   *sublink = (SubLink *) node;
		SubLink   *newnode;

		FLATCOPY(newnode, sublink, SubLink);
		MUTATE(newnode->lefthand, sublink->lefthand, List *,
			   apply_RIR_view_mutator, context);
		context->sublevels_up++;
		MUTATE(newnode->subselect, sublink->subselect, Node *,
			   apply_RIR_view_mutator, context);
		context->sublevels_up--;
		return (Node *) newnode;
	}
	if (IsA(node, Query))
	{
		Query  *query = (Query *) node;
		Query  *newnode;

		FLATCOPY(newnode, query, Query);
		MUTATE(newnode->targetList, query->targetList, List *,
			   apply_RIR_view_mutator, context);
		MUTATE(newnode->qual, query->qual, Node *,
			   apply_RIR_view_mutator, context);
		MUTATE(newnode->havingQual, query->havingQual, Node *,
			   apply_RIR_view_mutator, context);
		return (Node *) newnode;
	}
	return expression_tree_mutator(node, apply_RIR_view_mutator,
								   (void *) context);
}

static Node *
apply_RIR_view(Node *node, int rt_index, RangeTblEntry *rte, List *tlist,
			   int *modified, int sublevels_up)
{
	apply_RIR_view_context	context;

	context.rt_index = rt_index;
	context.sublevels_up = sublevels_up;
	context.rte = rte;
	context.tlist = tlist;
	context.modified = modified;

	return apply_RIR_view_mutator(node, &context);
}


static Query *
ApplyRetrieveRule(Query *parsetree,
				  RewriteRule *rule,
				  int rt_index,
				  int relation_level,
				  Relation relation,
				  bool relWasInJoinSet,
				  int *modified)
{
	Query	   *rule_action = NULL;
	Node	   *rule_qual;
	List	   *rtable,
			   *addedrtable,
			   *l;
	int			nothing,
				rt_length;
	int			badsql = false;

	rule_qual = rule->qual;
	if (rule->actions)
	{
		if (length(rule->actions) > 1)	/* ??? because we don't handle
										 * rules with more than one
										 * action? -ay */

			return parsetree;
		rule_action = copyObject(lfirst(rule->actions));
		nothing = FALSE;
	}
	else
		nothing = TRUE;

	rtable = copyObject(parsetree->rtable);
	rt_length = length(rtable);	/* original length, not counting rule */

	addedrtable = copyObject(rule_action->rtable);

	/* If the original rel wasn't in the join set, none of its spawn is.
	 * If it was, then leave the spawn's flags as they are.
	 */
	if (! relWasInJoinSet)
	{
		foreach(l, addedrtable)
		{
			RangeTblEntry *rte = lfirst(l);
			rte->inJoinSet = false;
		}
	}

	rtable = nconc(rtable, addedrtable);
	parsetree->rtable = rtable;

	/* FOR UPDATE of view... */
	foreach(l, parsetree->rowMark)
	{
		if (((RowMark *) lfirst(l))->rti == rt_index)
			break;
	}
	if (l != NULL)				/* oh, hell -:) */
	{
		RowMark    *newrm;
		Index		rti = 1;
		List	   *l2;

		CheckSelectForUpdate(rule_action);

		/*
		 * We believe that rt_index is VIEW - nothing should be marked for
		 * VIEW, but ACL check must be done. As for real tables of VIEW -
		 * their rows must be marked, but we have to skip ACL check for
		 * them.
		 */
		((RowMark *) lfirst(l))->info &= ~ROW_MARK_FOR_UPDATE;

		foreach(l2, rule_action->rtable)
		{

			/*
			 * RTable of VIEW has two entries of VIEW itself - we use
			 * relid to skip them.
			 */
			if (relation->rd_id != ((RangeTblEntry *) lfirst(l2))->relid)
			{
				newrm = makeNode(RowMark);
				newrm->rti = rti + rt_length;
				newrm->info = ROW_MARK_FOR_UPDATE;
				lnext(l) = lcons(newrm, lnext(l));
				l = lnext(l);
			}
			rti++;
		}
	}

	rule_action->rtable = rtable;
	OffsetVarNodes((Node *) rule_qual, rt_length, 0);
	OffsetVarNodes((Node *) rule_action, rt_length, 0);

	ChangeVarNodes((Node *) rule_qual,
				   PRS2_CURRENT_VARNO + rt_length, rt_index, 0);
	ChangeVarNodes((Node *) rule_action,
				   PRS2_CURRENT_VARNO + rt_length, rt_index, 0);

	if (relation_level)
	{
		RangeTblEntry  *rte = rt_fetch(rt_index, rtable);

		parsetree = (Query *) apply_RIR_view((Node *) parsetree,
											 rt_index, rte,
											 rule_action->targetList,
											 modified, 0);
		rule_action = (Query *) apply_RIR_view((Node *) rule_action,
											   rt_index, rte,
											   rule_action->targetList,
											   modified, 0);
	}
	else
	{
		HandleRIRAttributeRule(parsetree, rtable, rule_action->targetList,
							   rt_index, rule->attrno, modified, &badsql);
	}
	if (*modified && !badsql)
	{
		AddQual(parsetree, rule_action->qual);
		AddGroupClause(parsetree, rule_action->groupClause,
					   rule_action->targetList);
		AddHavingQual(parsetree, rule_action->havingQual);
		parsetree->hasAggs = (rule_action->hasAggs || parsetree->hasAggs);
		parsetree->hasSubLinks = (rule_action->hasSubLinks || parsetree->hasSubLinks);
	}

	return parsetree;
}


/*
 * fireRIRonSubselect -
 *	Apply fireRIRrules() to each subselect found in the given tree.
 *
 * NOTE: although this has the form of a walker, we cheat and modify the
 * SubLink nodes in-place.  It is caller's responsibility to ensure that
 * no unwanted side-effects occur!
 */
static bool
fireRIRonSubselect(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
	{
		SubLink    *sub = (SubLink *) node;
		Query	   *qry;

		/* Process lefthand args */
		if (fireRIRonSubselect((Node *) (sub->lefthand), context))
			return true;
		/* Do what we came for */
		qry = fireRIRrules((Query *) (sub->subselect));
		sub->subselect = (Node *) qry;
		/* Need not recurse into subselect, because fireRIRrules did it */
		return false;
	}
	if (IsA(node, Query))
	{
		/* Reach here when called from fireRIRrules */
		Query	   *qry = (Query *) node;

		if (fireRIRonSubselect((Node *) (qry->targetList), context))
			return true;
		if (fireRIRonSubselect((Node *) (qry->qual), context))
			return true;
		if (fireRIRonSubselect((Node *) (qry->havingQual), context))
			return true;
		return false;
	}
	return expression_tree_walker(node, fireRIRonSubselect,
								  (void *) context);
}


/*
 * fireRIRrules -
 *	Apply all RIR rules on each rangetable entry in a query
 */
static Query *
fireRIRrules(Query *parsetree)
{
	int			rt_index;
	RangeTblEntry *rte;
	Relation	rel;
	List	   *locks;
	RuleLock   *rules;
	RewriteRule *rule;
	RewriteRule RIRonly;
	bool		relWasInJoinSet;
	int			modified = false;
	int			i;
	List	   *l;

	/* don't try to convert this into a foreach loop, because
	 * rtable list can get changed each time through...
	 */
	rt_index = 0;
	while (rt_index < length(parsetree->rtable))
	{
		++rt_index;

		rte = rt_fetch(rt_index, parsetree->rtable);

		/*
		 * If the table is not one named in the original FROM clause
		 * then it must be referenced in the query, or we ignore it.
		 * This prevents infinite expansion loop due to new rtable
		 * entries inserted by expansion of a rule.
		 */
		if (! rte->inFromCl && rt_index != parsetree->resultRelation &&
			! rangeTableEntry_used((Node *) parsetree, rt_index, 0))
		{
			/* Make sure the planner ignores it too... */
			rte->inJoinSet = false;
			continue;
		}

		rel = heap_openr(rte->relname, AccessShareLock);
		rules = rel->rd_rules;
		if (rules == NULL)
		{
			heap_close(rel, AccessShareLock);
			continue;
		}

		relWasInJoinSet = rte->inJoinSet; /* save before possibly clearing */

		/*
		 * Collect the RIR rules that we must apply
		 */
		locks = NIL;
		for (i = 0; i < rules->numLocks; i++)
		{
			rule = rules->rules[i];
			if (rule->event != CMD_SELECT)
				continue;

			if (rule->attrno > 0)
			{
				/* per-attr rule; do we need it? */
				if (! attribute_used((Node *) parsetree,
									 rt_index,
									 rule->attrno, 0))
					continue;
			}
			else
			{
				/* Rel-wide ON SELECT DO INSTEAD means this is a view.
				 * Remove the view from the planner's join target set,
				 * or we'll get no rows out because view itself is empty!
				 */
				if (rule->isInstead)
					rte->inJoinSet = false;
			}

			locks = lappend(locks, rule);
		}

		/*
		 * Check permissions
		 */
		checkLockPerms(locks, parsetree, rt_index);

		/*
		 * Now apply them
		 */
		foreach(l, locks)
		{
			rule = lfirst(l);

			RIRonly.event = rule->event;
			RIRonly.attrno = rule->attrno;
			RIRonly.qual = rule->qual;
			RIRonly.actions = rule->actions;

			parsetree = ApplyRetrieveRule(parsetree,
										  &RIRonly,
										  rt_index,
										  RIRonly.attrno == -1,
										  rel,
										  relWasInJoinSet,
										  &modified);
		}

		heap_close(rel, AccessShareLock);
	}

	if (parsetree->hasAggs)
		parsetree->qual = modifyAggrefQual(parsetree->qual, parsetree);

	if (parsetree->hasSubLinks)
		fireRIRonSubselect((Node *) parsetree, NULL);

	return parsetree;
}


/*
 * idea is to fire regular rules first, then qualified instead
 * rules and unqualified instead rules last. Any lemming is counted for.
 */
static List *
orderRules(List *locks)
{
	List	   *regular = NIL;
	List	   *instead_rules = NIL;
	List	   *instead_qualified = NIL;
	List	   *i;

	foreach(i, locks)
	{
		RewriteRule *rule_lock = (RewriteRule *) lfirst(i);

		if (rule_lock->isInstead)
		{
			if (rule_lock->qual == NULL)
				instead_rules = lappend(instead_rules, rule_lock);
			else
				instead_qualified = lappend(instead_qualified, rule_lock);
		}
		else
			regular = lappend(regular, rule_lock);
	}
	regular = nconc(regular, instead_qualified);
	return nconc(regular, instead_rules);
}



static Query *
CopyAndAddQual(Query *parsetree,
			   List *actions,
			   Node *rule_qual,
			   int rt_index,
			   CmdType event)
{
	Query	   *new_tree = (Query *) copyObject(parsetree);
	Node	   *new_qual = NULL;
	Query	   *rule_action = NULL;

	if (actions)
		rule_action = lfirst(actions);
	if (rule_qual != NULL)
		new_qual = (Node *) copyObject(rule_qual);
	if (rule_action != NULL)
	{
		List	   *rtable;
		int			rt_length;

		rtable = new_tree->rtable;
		rt_length = length(rtable);
		rtable = nconc(rtable, copyObject(rule_action->rtable));
		new_tree->rtable = rtable;
		OffsetVarNodes(new_qual, rt_length, 0);
		ChangeVarNodes(new_qual, PRS2_CURRENT_VARNO + rt_length, rt_index, 0);
	}
	/* XXX -- where current doesn't work for instead nothing.... yet */
	AddNotQual(new_tree, new_qual);

	return new_tree;
}



/*
 *	fireRules -
 *	   Iterate through rule locks applying rules.
 *	   All rules create their own parsetrees. Instead rules
 *	   with rule qualification save the original parsetree
 *	   and add their negated qualification to it. Real instead
 *	   rules finally throw away the original parsetree.
 *
 *	   remember: reality is for dead birds -- glass
 *
 */
static List *
fireRules(Query *parsetree,
		  int rt_index,
		  CmdType event,
		  bool *instead_flag,
		  List *locks,
		  List **qual_products)
{
	RewriteInfo *info;
	List	   *results = NIL;
	List	   *i;

	/* choose rule to fire from list of rules */
	if (locks == NIL)
		return NIL;

	locks = orderRules(locks);	/* real instead rules last */
	foreach(i, locks)
	{
		RewriteRule *rule_lock = (RewriteRule *) lfirst(i);
		Node	   *qual,
				   *event_qual;
		List	   *actions;
		List	   *r;

		/*
		 * Instead rules change the resultRelation of the query. So the
		 * permission checks on the initial resultRelation would never be
		 * done (this is normally done in the executor deep down). So we
		 * must do it here. The result relations resulting from earlier
		 * rewrites are already checked against the rules eventrelation
		 * owner (during matchLocks) and have the skipAcl flag set.
		 */
		if (rule_lock->isInstead &&
			parsetree->commandType != CMD_SELECT)
		{
			RangeTblEntry *rte;
			int32		acl_rc;
			int32		reqperm;

			switch (parsetree->commandType)
			{
				case CMD_INSERT:
					reqperm = ACL_AP;
					break;
				default:
					reqperm = ACL_WR;
					break;
			}

			rte = rt_fetch(parsetree->resultRelation, parsetree->rtable);
			if (!rte->skipAcl)
			{
				acl_rc = pg_aclcheck(rte->relname,
									 GetPgUserName(), reqperm);
				if (acl_rc != ACLCHECK_OK)
				{
					elog(ERROR, "%s: %s",
						 rte->relname,
						 aclcheck_error_strings[acl_rc]);
				}
			}
		}

		/* multiple rule action time */
		*instead_flag = rule_lock->isInstead;
		event_qual = rule_lock->qual;
		actions = rule_lock->actions;
		if (event_qual != NULL && *instead_flag)
		{
			Query	   *qual_product;
			RewriteInfo qual_info;

			/* ----------
			 * If there are instead rules with qualifications,
			 * the original query is still performed. But all
			 * the negated rule qualifications of the instead
			 * rules are added so it does its actions only
			 * in cases where the rule quals of all instead
			 * rules are false. Think of it as the default
			 * action in a case. We save this in *qual_products
			 * so deepRewriteQuery() can add it to the query
			 * list after we mangled it up enough.
			 * ----------
			 */
			if (*qual_products == NIL)
				qual_product = parsetree;
			else
				qual_product = (Query *) nth(0, *qual_products);

			MemSet(&qual_info, 0, sizeof(qual_info));
			qual_info.event = qual_product->commandType;
			qual_info.current_varno = rt_index;
			qual_info.new_varno = length(qual_product->rtable) + 2;

			qual_product = CopyAndAddQual(qual_product,
										  actions,
										  event_qual,
										  rt_index,
										  event);

			qual_info.rule_action = qual_product;

			if (event == CMD_INSERT || event == CMD_UPDATE)
				FixNew(&qual_info, qual_product);

			*qual_products = lappend(NIL, qual_product);
		}

		foreach(r, actions)
		{
			Query	   *rule_action = lfirst(r);
			Node	   *rule_qual = copyObject(event_qual);

			if (rule_action->commandType == CMD_NOTHING)
				continue;

			/*--------------------------------------------------
			 * We copy the qualifications of the parsetree
			 * to the action and vice versa. So force
			 * hasSubLinks if one of them has it.
			 *
			 * As of 6.4 only parsetree qualifications can
			 * have sublinks. If this changes, we must make
			 * this a node lookup at the end of rewriting.
			 *
			 * Jan
			 *--------------------------------------------------
			 */
			if (parsetree->hasSubLinks && !rule_action->hasSubLinks)
			{
				rule_action = copyObject(rule_action);
				rule_action->hasSubLinks = TRUE;
			}
			if (!parsetree->hasSubLinks && rule_action->hasSubLinks)
				parsetree->hasSubLinks = TRUE;

			/*--------------------------------------------------
			 * Step 1:
			 *	  Rewrite current.attribute or current to tuple variable
			 *	  this appears to be done in parser?
			 *--------------------------------------------------
			 */
			info = gatherRewriteMeta(parsetree, rule_action, rule_qual,
									 rt_index, event, instead_flag);

			/* handle escapable cases, or those handled by other code */
			if (info->nothing)
			{
				if (*instead_flag)
					return NIL;
				else
					continue;
			}

			if (info->action == info->event &&
				info->event == CMD_SELECT)
				continue;

			/*
			 * Event Qualification forces copying of parsetree and
			 * splitting into two queries one w/rule_qual, one w/NOT
			 * rule_qual. Also add user query qual onto rule action
			 */
			qual = parsetree->qual;
			AddQual(info->rule_action, qual);

			if (info->rule_qual != NULL)
				AddQual(info->rule_action, info->rule_qual);

			/*--------------------------------------------------
			 * Step 2:
			 *	  Rewrite new.attribute w/ right hand side of target-list
			 *	  entry for appropriate field name in insert/update
			 *--------------------------------------------------
			 */
			if ((info->event == CMD_INSERT) || (info->event == CMD_UPDATE))
				FixNew(info, parsetree);

			/*--------------------------------------------------
			 * Step 3:
			 *	  rewriting due to retrieve rules
			 *--------------------------------------------------
			 */
			info->rule_action->rtable = info->rt;

			/*
			 * ProcessRetrieveQuery(info->rule_action, info->rt,
			 * &orig_instead_flag, TRUE);
			 */

			/*--------------------------------------------------
			 * Step 4
			 *	  Simplify? hey, no algorithm for simplification... let
			 *	  the planner do it.
			 *--------------------------------------------------
			 */
			results = lappend(results, info->rule_action);

			pfree(info);
		}

		/* ----------
		 * If this was an unqualified instead rule,
		 * throw away an eventually saved 'default' parsetree
		 * ----------
		 */
		if (event_qual == NULL && *instead_flag)
			*qual_products = NIL;
	}
	return results;
}



static List *
RewriteQuery(Query *parsetree, bool *instead_flag, List **qual_products)
{
	CmdType		event;
	List	   *product_queries = NIL;
	int			result_relation = 0;
	RangeTblEntry *rt_entry;
	Relation	rt_entry_relation = NULL;
	RuleLock   *rt_entry_locks = NULL;

	Assert(parsetree != NULL);

	event = parsetree->commandType;

	/*
	 * SELECT rules are handled later when we have all the queries that
	 * should get executed
	 */
	if (event == CMD_SELECT)
		return NIL;

	/*
	 * Utilities aren't rewritten at all - why is this here?
	 */
	if (event == CMD_UTILITY)
		return NIL;

	/*
	 * the statement is an update, insert or delete - fire rules on it.
	 */
	result_relation = parsetree->resultRelation;
	rt_entry = rt_fetch(result_relation, parsetree->rtable);
	rt_entry_relation = heap_openr(rt_entry->relname, AccessShareLock);
	rt_entry_locks = rt_entry_relation->rd_rules;
	heap_close(rt_entry_relation, AccessShareLock);

	if (rt_entry_locks != NULL)
	{
		List	   *locks = matchLocks(event, rt_entry_locks, result_relation, parsetree);

		product_queries = fireRules(parsetree,
									result_relation,
									event,
									instead_flag,
									locks,
									qual_products);
	}

	return product_queries;
}


/*
 * to avoid infinite recursion, we restrict the number of times a query
 * can be rewritten. Detecting cycles is left for the reader as an excercise.
 */
#ifndef REWRITE_INVOKE_MAX
#define REWRITE_INVOKE_MAX		10
#endif

static int	numQueryRewriteInvoked = 0;

/*
 * deepRewriteQuery -
 *	  rewrites the query and apply the rules again on the queries rewritten
 */
static List *
deepRewriteQuery(Query *parsetree)
{
	List	   *n;
	List	   *rewritten = NIL;
	List	   *result = NIL;
	bool		instead;
	List	   *qual_products = NIL;



	if (++numQueryRewriteInvoked > REWRITE_INVOKE_MAX)
	{
		elog(ERROR, "query rewritten %d times, may contain cycles",
			 numQueryRewriteInvoked - 1);
	}

	instead = FALSE;
	result = RewriteQuery(parsetree, &instead, &qual_products);

	foreach(n, result)
	{
		Query	   *pt = lfirst(n);
		List	   *newstuff = NIL;

		newstuff = deepRewriteQuery(pt);
		if (newstuff != NIL)
			rewritten = nconc(rewritten, newstuff);
	}

	/* ----------
	 * qual_products are the original query with the negated
	 * rule qualification of an instead rule
	 * ----------
	 */
	if (qual_products != NIL)
		rewritten = nconc(rewritten, qual_products);

	/* ----------
	 * The original query is appended last if not instead
	 * because update and delete rule actions might not do
	 * anything if they are invoked after the update or
	 * delete is performed. The command counter increment
	 * between the query execution makes the deleted (and
	 * maybe the updated) tuples disappear so the scans
	 * for them in the rule actions cannot find them.
	 * ----------
	 */
	if (!instead)
		rewritten = lappend(rewritten, parsetree);

	return rewritten;
}


/*
 * QueryOneRewrite -
 *	  rewrite one query
 */
static List *
QueryRewriteOne(Query *parsetree)
{
	numQueryRewriteInvoked = 0;

	/*
	 * take a deep breath and apply all the rewrite rules - ay
	 */
	return deepRewriteQuery(parsetree);
}


/*
 * BasicQueryRewrite -
 *	  rewrite one query via query rewrite system, possibly returning 0
 *	  or many queries
 */
static List *
BasicQueryRewrite(Query *parsetree)
{
	List	   *querylist;
	List	   *results = NIL;
	List	   *l;
	Query	   *query;

	/*
	 * Step 1
	 *
	 * Apply all non-SELECT rules possibly getting 0 or many queries
	 */
	querylist = QueryRewriteOne(parsetree);

	/*
	 * Step 2
	 *
	 * Apply all the RIR rules on each query
	 */
	foreach(l, querylist)
	{
		query = fireRIRrules((Query *) lfirst(l));

		/*
		 * If the query was marked having aggregates, check if this is
		 * still true after rewriting.  Ditto for sublinks.  Note there
		 * should be no aggs in the qual at this point.
		 */
		if (query->hasAggs)
		{
			query->hasAggs =
				checkExprHasAggs((Node *) (query->targetList)) ||
				checkExprHasAggs((Node *) (query->havingQual));
			if (checkExprHasAggs((Node *) (query->qual)))
				elog(ERROR, "BasicQueryRewrite: failed to remove aggs from qual");
		}
		if (query->hasSubLinks)
			query->hasSubLinks =
				checkExprHasSubLink((Node *) (query->targetList)) ||
				checkExprHasSubLink((Node *) (query->qual)) ||
				checkExprHasSubLink((Node *) (query->havingQual));
		results = lappend(results, query);
	}

	return results;
}

/*
 * QueryRewrite -
 *	  Primary entry point to the query rewriter.
 *	  Rewrite one query via query rewrite system, possibly returning 0
 *	  or many queries.
 *
 * NOTE: The code in QueryRewrite was formerly in pg_parse_and_plan(), and was
 * moved here so that it would be invoked during EXPLAIN.  The division of
 * labor between this routine and BasicQueryRewrite is not obviously correct
 * ... at least not to me ... tgl 5/99.
 */
List *
QueryRewrite(Query *parsetree)
{
	List	   *rewritten,
			   *rewritten_item;

	/*
	 * Rewrite Union, Intersect and Except Queries to normal Union Queries
	 * using IN and NOT IN subselects
	 */
	if (parsetree->intersectClause)
		parsetree = Except_Intersect_Rewrite(parsetree);

	/* Rewrite basic queries (retrieve, append, delete, replace) */
	rewritten = BasicQueryRewrite(parsetree);

	/*
	 * Rewrite the UNIONS.
	 */
	foreach(rewritten_item, rewritten)
	{
		Query	   *qry = (Query *) lfirst(rewritten_item);
		List	   *union_result = NIL;
		List	   *union_item;

		foreach(union_item, qry->unionClause)
		{
			union_result = nconc(union_result,
						BasicQueryRewrite((Query *) lfirst(union_item)));
		}
		qry->unionClause = union_result;
	}

	return rewritten;
}

/* This function takes two targetlists as arguments and checks if the
 * targetlists are compatible (i.e. both select for the same number of
 * attributes and the types are compatible */
static void
check_targetlists_are_compatible(List *prev_target, List *current_target)
{
	List	   *tl,
			   *next_target;
	int			prev_len = 0,
				next_len = 0;

	foreach(tl, prev_target)
		if (!((TargetEntry *) lfirst(tl))->resdom->resjunk)
		prev_len++;

	foreach(next_target, current_target)
		if (!((TargetEntry *) lfirst(next_target))->resdom->resjunk)
		next_len++;

	if (prev_len != next_len)
		elog(ERROR, "Each UNION | EXCEPT | INTERSECT query must have the same number of columns.");

	foreach(next_target, current_target)
	{
		Oid			itype;
		Oid			otype;

		otype = ((TargetEntry *) lfirst(prev_target))->resdom->restype;
		itype = ((TargetEntry *) lfirst(next_target))->resdom->restype;

		/* one or both is a NULL column? then don't convert... */
		if (otype == InvalidOid)
		{
			/* propagate a known type forward, if available */
			if (itype != InvalidOid)
				((TargetEntry *) lfirst(prev_target))->resdom->restype = itype;
#ifdef NOT_USED
			else
			{
				((TargetEntry *) lfirst(prev_target))->resdom->restype = UNKNOWNOID;
				((TargetEntry *) lfirst(next_target))->resdom->restype = UNKNOWNOID;
			}
#endif
		}
		else if (itype == InvalidOid)
		{
		}
		/* they don't match in type? then convert... */
		else if (itype != otype)
		{
			Node	   *expr;

			expr = ((TargetEntry *) lfirst(next_target))->expr;
			expr = CoerceTargetExpr(NULL, expr, itype, otype, -1);
			if (expr == NULL)
			{
				elog(ERROR, "Unable to transform %s to %s"
					 "\n\tEach UNION | EXCEPT | INTERSECT clause must have compatible target types",
					 typeidTypeName(itype),
					 typeidTypeName(otype));
			}
			((TargetEntry *) lfirst(next_target))->expr = expr;
			((TargetEntry *) lfirst(next_target))->resdom->restype = otype;
		}

		/* both are UNKNOWN? then evaluate as text... */
		else if (itype == UNKNOWNOID)
		{
			((TargetEntry *) lfirst(next_target))->resdom->restype = TEXTOID;
			((TargetEntry *) lfirst(prev_target))->resdom->restype = TEXTOID;
		}
		prev_target = lnext(prev_target);
	}
}

/* Rewrites UNION INTERSECT and EXCEPT queries to semantiacally equivalent
 * queries that use IN and NOT IN subselects.
 *
 * The operator tree is attached to 'intersectClause' (see rule
 * 'SelectStmt' in gram.y) of the 'parsetree' given as an
 * argument. First we remember some clauses (the sortClause, the
 * distinctClause etc.)  Then we translate the operator tree to DNF
 * (disjunctive normal form) by 'cnfify'. (Note that 'cnfify' produces
 * CNF but as we exchanged ANDs with ORs in function A_Expr_to_Expr()
 * earlier we get DNF after exchanging ANDs and ORs again in the
 * result.) Now we create a new query by evaluating the new operator
 * tree which is in DNF now. For every AND we create an entry in the
 * union list and for every OR we create an IN subselect. (NOT IN
 * subselects are created for OR NOT nodes). The first entry of the
 * union list is handed back but before that the remembered clauses
 * (sortClause etc) are attached to the new top Node (Note that the
 * new top Node can differ from the parsetree given as argument because of
 * the translation to DNF. That's why we have to remember the sortClause
 * and so on!) */
static Query *
Except_Intersect_Rewrite(Query *parsetree)
{

	SubLink    *n;
	Query	   *result,
			   *intersect_node;
	List	   *elist,
			   *intersect_list = NIL,
			   *intersect,
			   *intersectClause;
	List	   *union_list = NIL,
			   *sortClause,
			   *distinctClause;
	List	   *left_expr,
			   *right_expr,
			   *resnames = NIL;
	char	   *op,
			   *into;
	bool		isBinary,
				isPortal,
				isTemp;
	Node	   *limitOffset,
			   *limitCount;
	CmdType		commandType = CMD_SELECT;
	RangeTblEntry *rtable_insert = NULL;
	List	   *prev_target = NIL;

	/*
	 * Remember the Resnames of the given parsetree's targetlist (these
	 * are the resnames of the first Select Statement of the query
	 * formulated by the user and he wants the columns named by these
	 * strings. The transformation to DNF can cause another Select
	 * Statment to be the top one which uses other names for its columns.
	 * Therefore we remeber the original names and attach them to the
	 * targetlist of the new topmost Node at the end of this function
	 */
	foreach(elist, parsetree->targetList)
	{
		TargetEntry *tent = (TargetEntry *) lfirst(elist);

		resnames = lappend(resnames, tent->resdom->resname);
	}

	/*
	 * If the Statement is an INSERT INTO ... (SELECT...) statement using
	 * UNIONs, INTERSECTs or EXCEPTs and the transformation to DNF makes
	 * another Node to the top node we have to transform the new top node
	 * to an INSERT node and the original INSERT node to a SELECT node
	 */
	if (parsetree->commandType == CMD_INSERT)
	{
		/*
		 * The result relation ( = the one to insert into) has to be
		 * attached to the rtable list of the new top node
		 */
		rtable_insert = rt_fetch(parsetree->resultRelation, parsetree->rtable);

		parsetree->commandType = CMD_SELECT;
		commandType = CMD_INSERT;
		parsetree->resultRelation = 0;
	}

	/*
	 * Save some items, to be able to attach them to the resulting top
	 * node at the end of the function
	 */
	sortClause = parsetree->sortClause;
	distinctClause = parsetree->distinctClause;
	into = parsetree->into;
	isBinary = parsetree->isBinary;
	isPortal = parsetree->isPortal;
	isTemp = parsetree->isTemp;
	limitOffset = parsetree->limitOffset;
	limitCount = parsetree->limitCount;

	/*
	 * The operator tree attached to parsetree->intersectClause is still
	 * 'raw' ( = the leaf nodes are still SelectStmt nodes instead of
	 * Query nodes) So step through the tree and transform the nodes using
	 * parse_analyze().
	 *
	 * The parsetree (given as an argument to Except_Intersect_Rewrite()) has
	 * already been transformed and transforming it again would cause
	 * troubles.  So we give the 'raw' version (of the cooked parsetree)
	 * to the function to prevent an additional transformation. Instead we
	 * hand back the 'cooked' version also given as an argument to
	 * intersect_tree_analyze()
	 */
	intersectClause =
		(List *) intersect_tree_analyze((Node *) parsetree->intersectClause,
								 (Node *) lfirst(parsetree->unionClause),
										(Node *) parsetree);

	/* intersectClause is no longer needed so set it to NIL */
	parsetree->intersectClause = NIL;

	/*
	 * unionClause will be needed later on but the list it delivered is no
	 * longer needed, so set it to NIL
	 */
	parsetree->unionClause = NIL;

	/*
	 * Transform the operator tree to DNF (remember ANDs and ORs have been
	 * exchanged, that's why we get DNF by using cnfify)
	 *
	 * After the call, explicit ANDs are removed and all AND operands are
	 * simply items in the intersectClause list
	 */
	intersectClause = cnfify((Expr *) intersectClause, true);

	/*
	 * For every entry of the intersectClause list we generate one entry
	 * in the union_list
	 */
	foreach(intersect, intersectClause)
	{

		/*
		 * for every OR we create an IN subselect and for every OR NOT we
		 * create a NOT IN subselect, so first extract all the Select
		 * Query nodes from the tree (that contains only OR or OR NOTs any
		 * more because we did a transformation to DNF
		 *
		 * There must be at least one node that is not negated (i.e. just OR
		 * and not OR NOT) and this node will be the first in the list
		 * returned
		 */
		intersect_list = NIL;
		create_intersect_list((Node *) lfirst(intersect), &intersect_list);

		/*
		 * This one will become the Select Query node, all other nodes are
		 * transformed into subselects under this node!
		 */
		intersect_node = (Query *) lfirst(intersect_list);
		intersect_list = lnext(intersect_list);

		/*
		 * Check if all Select Statements use the same number of
		 * attributes and if all corresponding attributes are of the same
		 * type
		 */
		if (prev_target)
			check_targetlists_are_compatible(prev_target, intersect_node->targetList);
		prev_target = intersect_node->targetList;
		/* End of check for corresponding targetlists */

		/*
		 * Transform all nodes remaining into subselects and add them to
		 * the qualifications of the Select Query node
		 */
		while (intersect_list != NIL)
		{

			n = makeNode(SubLink);

			/* Here we got an OR so transform it to an IN subselect */
			if (IsA(lfirst(intersect_list), Query))
			{

				/*
				 * Check if all Select Statements use the same number of
				 * attributes and if all corresponding attributes are of
				 * the same type
				 */
				check_targetlists_are_compatible(prev_target,
						 ((Query *) lfirst(intersect_list))->targetList);
				/* End of check for corresponding targetlists */

				n->subselect = lfirst(intersect_list);
				op = "=";
				n->subLinkType = ANY_SUBLINK;
				n->useor = false;
			}

			/*
			 * Here we got an OR NOT node so transform it to a NOT IN
			 * subselect
			 */
			else
			{

				/*
				 * Check if all Select Statements use the same number of
				 * attributes and if all corresponding attributes are of
				 * the same type
				 */
				check_targetlists_are_compatible(prev_target,
												 ((Query *) lfirst(((Expr *) lfirst(intersect_list))->args))->targetList);
				/* End of check for corresponding targetlists */

				n->subselect = (Node *) lfirst(((Expr *) lfirst(intersect_list))->args);
				op = "<>";
				n->subLinkType = ALL_SUBLINK;
				n->useor = true;
			}

			/*
			 * Prepare the lefthand side of the Sublinks: All the entries
			 * of the targetlist must be (IN) or must not be (NOT IN) the
			 * subselect
			 */
			n->lefthand = NIL;
			foreach(elist, intersect_node->targetList)
			{
				TargetEntry *tent = (TargetEntry *) lfirst(elist);

				n->lefthand = lappend(n->lefthand, tent->expr);
			}

			/*
			 * Also prepare the list of Opers that must be used for the
			 * comparisons (they depend on the specific datatypes involved!)
			 */
			left_expr = n->lefthand;
			right_expr = ((Query *) (n->subselect))->targetList;
			n->oper = NIL;

			foreach(elist, left_expr)
			{
				Node	   *lexpr = lfirst(elist);
				TargetEntry *tent = (TargetEntry *) lfirst(right_expr);
				Operator	optup;
				Form_pg_operator opform;
				Oper	   *newop;

				optup = oper(op,
							 exprType(lexpr),
							 exprType(tent->expr),
							 FALSE);
				opform = (Form_pg_operator) GETSTRUCT(optup);

				if (opform->oprresult != BOOLOID)
					elog(ERROR, "parser: '%s' must return 'bool' to be used with quantified predicate subquery", op);

				newop = makeOper(oprid(optup),/* opno */
								 InvalidOid, /* opid */
								 opform->oprresult,
								 0,
								 NULL);

				n->oper = lappend(n->oper, newop);

				right_expr = lnext(right_expr);
			}

			/*
			 * If the Select Query node has aggregates in use add all the
			 * subselects to the HAVING qual else to the WHERE qual
			 */
			if (intersect_node->hasAggs)
				AddHavingQual(intersect_node, (Node *) n);
			else
				AddQual(intersect_node, (Node *) n);

			/* Now we got sublinks */
			intersect_node->hasSubLinks = true;
			intersect_list = lnext(intersect_list);
		}
		intersect_node->intersectClause = NIL;
		union_list = lappend(union_list, intersect_node);
	}

	/* The first entry to union_list is our new top node */
	result = (Query *) lfirst(union_list);
	/* attach the rest to unionClause */
	result->unionClause = lnext(union_list);
	/* Attach all the items remembered in the beginning of the function */
	result->sortClause = sortClause;
	result->distinctClause = distinctClause;
	result->into = into;
	result->isPortal = isPortal;
	result->isBinary = isBinary;
	result->isTemp = isTemp;
	result->limitOffset = limitOffset;
	result->limitCount = limitCount;

	/*
	 * The relation to insert into is attached to the range table of the
	 * new top node
	 */
	if (commandType == CMD_INSERT)
	{
		result->rtable = lappend(result->rtable, rtable_insert);
		result->resultRelation = length(result->rtable);
		result->commandType = commandType;
	}

	/*
	 * The resnames of the originally first SelectStatement are attached
	 * to the new first SelectStatement
	 */
	foreach(elist, result->targetList)
	{
		TargetEntry *tent = (TargetEntry *) lfirst(elist);

		tent->resdom->resname = lfirst(resnames);
		resnames = lnext(resnames);
	}

	return result;
}

/* Create a list of nodes that are either Query nodes of NOT Expr
 * nodes followed by a Query node. The tree given in ptr contains at
 * least one non negated Query node. This node is attached to the
 * beginning of the list */

static void
create_intersect_list(Node *ptr, List **intersect_list)
{
	List	   *arg;

	if (IsA(ptr, Query))
	{
		/* The non negated node is attached at the beginning (lcons) */
		*intersect_list = lcons(ptr, *intersect_list);
		return;
	}

	if (IsA(ptr, Expr))
	{
		if (((Expr *) ptr)->opType == NOT_EXPR)
		{
			/* negated nodes are appended to the end (lappend) */
			*intersect_list = lappend(*intersect_list, ptr);
			return;
		}
		else
		{
			foreach(arg, ((Expr *) ptr)->args)
				create_intersect_list(lfirst(arg), intersect_list);
			return;
		}
		return;
	}
}

/* The nodes given in 'tree' are still 'raw' so 'cook' them using parse_analyze().
 * The node given in first_select has already been cooked, so don't transform
 * it again but return a pointer to the previously cooked version given in 'parsetree'
 * instead. */
static Node *
intersect_tree_analyze(Node *tree, Node *first_select, Node *parsetree)
{
	Node	   *result = (Node *) NIL;
	List	   *arg;

	if (IsA(tree, SelectStmt))
	{

		/*
		 * If we get to the tree given in first_select return parsetree
		 * instead of performing parse_analyze()
		 */
		if (tree == first_select)
			result = parsetree;
		else
		{
			/* transform the 'raw' nodes to 'cooked' Query nodes */
			List	   *qtree = parse_analyze(lcons(tree, NIL), NULL);

			result = (Node *) lfirst(qtree);
		}
	}

	if (IsA(tree, Expr))
	{
		/* Call recursively for every argument of the node */
		foreach(arg, ((Expr *) tree)->args)
			lfirst(arg) = intersect_tree_analyze(lfirst(arg), first_select, parsetree);
		result = tree;
	}
	return result;
}
