/*-------------------------------------------------------------------------
 *
 * rewriteManip.c
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteManip.c,v 1.45 2000/03/16 03:23:18 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "parser/parse_clause.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


/* macros borrowed from expression_tree_mutator */

#define FLATCOPY(newnode, node, nodetype)  \
	( (newnode) = makeNode(nodetype), \
	  memcpy((newnode), (node), sizeof(nodetype)) )

#define MUTATE(newfield, oldfield, fieldtype, mutator, context)  \
		( (newfield) = (fieldtype) mutator((Node *) (oldfield), (context)) )

static bool checkExprHasAggs_walker(Node *node, void *context);
static bool checkExprHasSubLink_walker(Node *node, void *context);


/*
 * checkExprHasAggs -
 *	Queries marked hasAggs might not have them any longer after
 *	rewriting. Check it.
 */
bool
checkExprHasAggs(Node *node)
{
	return checkExprHasAggs_walker(node, NULL);
}

static bool
checkExprHasAggs_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
		return true;			/* abort the tree traversal and return true */
	return expression_tree_walker(node, checkExprHasAggs_walker, context);
}

/*
 * checkExprHasSubLink -
 *	Queries marked hasSubLinks might not have them any longer after
 *	rewriting. Check it.
 */
bool
checkExprHasSubLink(Node *node)
{
	return checkExprHasSubLink_walker(node, NULL);
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
 * OffsetVarNodes - adjust Vars when appending one query's RT to another
 *
 * Find all Var nodes in the given tree with varlevelsup == sublevels_up,
 * and increment their varno fields (rangetable indexes) by 'offset'.
 * The varnoold fields are adjusted similarly.
 *
 * NOTE: although this has the form of a walker, we cheat and modify the
 * Var nodes in-place.  The given expression tree should have been copied
 * earlier to ensure that no unwanted side-effects occur!
 */

typedef struct {
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
	if (IsA(node, SubLink))
	{
		/*
		 * Standard expression_tree_walker will not recurse into subselect,
		 * but here we must do so.
		 */
		SubLink    *sub = (SubLink *) node;

		if (OffsetVarNodes_walker((Node *) (sub->lefthand),
								  context))
			return true;
		OffsetVarNodes((Node *) (sub->subselect),
					   context->offset,
					   context->sublevels_up + 1);
		return false;
	}
	if (IsA(node, Query))
	{
		/* Reach here after recursing down into subselect above... */
		Query	   *qry = (Query *) node;

		if (OffsetVarNodes_walker((Node *) (qry->targetList),
								  context))
			return true;
		if (OffsetVarNodes_walker((Node *) (qry->qual),
								  context))
			return true;
		if (OffsetVarNodes_walker((Node *) (qry->havingQual),
								  context))
			return true;
		return false;
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
	OffsetVarNodes_walker(node, &context);
}

/*
 * ChangeVarNodes - adjust Var nodes for a specific change of RT index
 *
 * Find all Var nodes in the given tree belonging to a specific relation
 * (identified by sublevels_up and rt_index), and change their varno fields
 * to 'new_index'.  The varnoold fields are changed too.
 *
 * NOTE: although this has the form of a walker, we cheat and modify the
 * Var nodes in-place.  The given expression tree should have been copied
 * earlier to ensure that no unwanted side-effects occur!
 */

typedef struct {
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
	if (IsA(node, SubLink))
	{
		/*
		 * Standard expression_tree_walker will not recurse into subselect,
		 * but here we must do so.
		 */
		SubLink    *sub = (SubLink *) node;

		if (ChangeVarNodes_walker((Node *) (sub->lefthand),
								  context))
			return true;
		ChangeVarNodes((Node *) (sub->subselect),
					   context->rt_index,
					   context->new_index,
					   context->sublevels_up + 1);
		return false;
	}
	if (IsA(node, Query))
	{
		/* Reach here after recursing down into subselect above... */
		Query	   *qry = (Query *) node;

		if (ChangeVarNodes_walker((Node *) (qry->targetList),
								  context))
			return true;
		if (ChangeVarNodes_walker((Node *) (qry->qual),
								  context))
			return true;
		if (ChangeVarNodes_walker((Node *) (qry->havingQual),
								  context))
			return true;
		return false;
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
	ChangeVarNodes_walker(node, &context);
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
 * NOTE: although this has the form of a walker, we cheat and modify the
 * Var nodes in-place.  The given expression tree should have been copied
 * earlier to ensure that no unwanted side-effects occur!
 */

typedef struct {
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
		return false;
	}
	if (IsA(node, SubLink))
	{
		/*
		 * Standard expression_tree_walker will not recurse into subselect,
		 * but here we must do so.
		 */
		SubLink    *sub = (SubLink *) node;

		if (IncrementVarSublevelsUp_walker((Node *) (sub->lefthand),
										   context))
			return true;
		IncrementVarSublevelsUp((Node *) (sub->subselect),
								context->delta_sublevels_up,
								context->min_sublevels_up + 1);
		return false;
	}
	if (IsA(node, Query))
	{
		/* Reach here after recursing down into subselect above... */
		Query	   *qry = (Query *) node;

		if (IncrementVarSublevelsUp_walker((Node *) (qry->targetList),
										   context))
			return true;
		if (IncrementVarSublevelsUp_walker((Node *) (qry->qual),
										   context))
			return true;
		if (IncrementVarSublevelsUp_walker((Node *) (qry->havingQual),
										   context))
			return true;
		return false;
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
	IncrementVarSublevelsUp_walker(node, &context);
}

/*
 * Add the given qualifier condition to the query's WHERE clause
 */
void
AddQual(Query *parsetree, Node *qual)
{
	Node	   *copy,
			   *old;

	if (qual == NULL)
		return;

	/* INTERSECT want's the original, but we need to copy - Jan */
	copy = copyObject(qual);

	old = parsetree->qual;
	if (old == NULL)
		parsetree->qual = copy;
	else
		parsetree->qual = (Node *) make_andclause(makeList(old, copy, -1));

	/*
	 * Make sure query is marked correctly if added qual has sublinks or
	 * aggregates (not sure it can ever have aggs, but sublinks definitely).
	 */
	parsetree->hasAggs |= checkExprHasAggs(copy);
	parsetree->hasSubLinks |= checkExprHasSubLink(copy);
}

/*
 * Add the given havingQual to the one already contained in the parsetree
 * just as AddQual does for the normal 'where' qual
 */
void
AddHavingQual(Query *parsetree, Node *havingQual)
{
	Node	   *copy,
			   *old;

	if (havingQual == NULL)
		return;

	/* INTERSECT want's the original, but we need to copy - Jan */
	copy = copyObject(havingQual);

	old = parsetree->havingQual;
	if (old == NULL)
		parsetree->havingQual = copy;
	else
		parsetree->havingQual = (Node *) make_andclause(makeList(old, copy, -1));

	/*
	 * Make sure query is marked correctly if added qual has sublinks or
	 * aggregates (not sure it can ever have aggs, but sublinks definitely).
	 */
	parsetree->hasAggs |= checkExprHasAggs(copy);
	parsetree->hasSubLinks |= checkExprHasSubLink(copy);
}

#ifdef NOT_USED
void
AddNotHavingQual(Query *parsetree, Node *havingQual)
{
	Node	   *notqual;

	if (havingQual == NULL)
		return;

	/* Need not copy input qual, because AddHavingQual will... */
	notqual = (Node *) make_notclause((Expr *) havingQual);

	AddHavingQual(parsetree, notqual);
}
#endif

void
AddNotQual(Query *parsetree, Node *qual)
{
	Node	   *notqual;

	if (qual == NULL)
		return;

	/* Need not copy input qual, because AddQual will... */
	notqual = (Node *) make_notclause((Expr *) qual);

	AddQual(parsetree, notqual);
}


/*
 * Add all expressions used by the given GroupClause list to the
 * parsetree's targetlist and groupclause list.
 *
 * tlist is the old targetlist associated with the input groupclauses.
 *
 * XXX shouldn't we be checking to see if there are already matching
 * entries in parsetree->targetlist?
 */
void
AddGroupClause(Query *parsetree, List *group_by, List *tlist)
{
	List	   *l;

	foreach(l, group_by)
	{
		GroupClause *groupclause = (GroupClause *) copyObject(lfirst(l));
		TargetEntry *tle = get_sortgroupclause_tle(groupclause, tlist);

		/* copy the groupclause's TLE from the old tlist */
		tle = (TargetEntry *) copyObject(tle);

		/* The ressortgroupref number in the old tlist might be already
		 * taken in the new tlist, so force assignment of a new number.
		 */
		tle->resdom->ressortgroupref = 0;
		groupclause->tleSortGroupRef =
			assignSortGroupRef(tle, parsetree->targetList);

		/* Also need to set the resno and mark it resjunk. */
		tle->resdom->resno = length(parsetree->targetList) + 1;
		tle->resdom->resjunk = true;

		parsetree->targetList = lappend(parsetree->targetList, tle);
		parsetree->groupClause = lappend(parsetree->groupClause, groupclause);
	}
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

#ifdef NOT_USED
void
FixResdomTypes(List *tlist)
{
	List	   *i;

	foreach(i, tlist)
	{
		TargetEntry *tle = lfirst(i);

		if (nodeTag(tle->expr) == T_Var)
		{
			Var		   *var = (Var *) tle->expr;

			tle->resdom->restype = var->vartype;
			tle->resdom->restypmod = var->vartypmod;
		}
	}
}

#endif

/* Find a targetlist entry by resno */
static Node *
FindMatchingNew(List *tlist, int attno)
{
	List	   *i;

	foreach(i, tlist)
	{
		TargetEntry *tle = lfirst(i);

		if (tle->resdom->resno == attno)
			return tle->expr;
	}
	return NULL;
}

/* Find a targetlist entry by resname */
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
			return tle->expr;
	}
	return NULL;
}


/*
 * ResolveNew - replace Vars with corresponding items from a targetlist
 *
 * Vars matching info->new_varno and sublevels_up are replaced by the
 * entry with matching resno from targetlist, if there is one.
 */

typedef struct {
	RewriteInfo	   *info;
	List		   *targetlist;
	int				sublevels_up;
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

		if (this_varno == context->info->new_varno &&
			this_varlevelsup == context->sublevels_up)
		{
			Node	   *n = FindMatchingNew(context->targetlist,
											var->varattno);

			if (n == NULL)
			{
				if (context->info->event == CMD_UPDATE)
				{
					/* For update, just change unmatched var's varno */
					n = copyObject(node);
					((Var *) n)->varno = context->info->current_varno;
					((Var *) n)->varnoold = context->info->current_varno;
					return n;
				}
				else
				{
					/* Otherwise replace unmatched var with a null */
					return make_null(var->vartype);
				}
			}
			else
			{
				/* Make a copy of the tlist item to return */
				n = copyObject(n);
				/* Adjust varlevelsup if tlist item is from higher query */
				if (this_varlevelsup > 0)
					IncrementVarSublevelsUp(n, this_varlevelsup, 0);
				return n;
			}
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
			   ResolveNew_mutator, context);
		context->sublevels_up++;
		MUTATE(newnode->subselect, sublink->subselect, Node *,
			   ResolveNew_mutator, context);
		context->sublevels_up--;
		return (Node *) newnode;
	}
	if (IsA(node, Query))
	{
		Query  *query = (Query *) node;
		Query  *newnode;

		/*
		 * XXX original code for ResolveNew only recursed into qual field
		 * of subquery.  I'm assuming that was an oversight ... tgl 9/99
		 */

		FLATCOPY(newnode, query, Query);
		MUTATE(newnode->targetList, query->targetList, List *,
			   ResolveNew_mutator, context);
		MUTATE(newnode->qual, query->qual, Node *,
			   ResolveNew_mutator, context);
		MUTATE(newnode->havingQual, query->havingQual, Node *,
			   ResolveNew_mutator, context);
		return (Node *) newnode;
	}
	return expression_tree_mutator(node, ResolveNew_mutator,
								   (void *) context);
}

static Node *
ResolveNew(Node *node, RewriteInfo *info, List *targetlist,
		   int sublevels_up)
{
	ResolveNew_context	context;

	context.info = info;
	context.targetlist = targetlist;
	context.sublevels_up = sublevels_up;

	return ResolveNew_mutator(node, &context);
}

void
FixNew(RewriteInfo *info, Query *parsetree)
{
	info->rule_action->targetList = (List *)
		ResolveNew((Node *) info->rule_action->targetList,
				   info, parsetree->targetList, 0);
	info->rule_action->qual = ResolveNew(info->rule_action->qual,
										 info, parsetree->targetList, 0);
	/* XXX original code didn't fix havingQual; presumably an oversight? */
	info->rule_action->havingQual = ResolveNew(info->rule_action->havingQual,
											   info, parsetree->targetList, 0);
}

/*
 * HandleRIRAttributeRule
 *	Replace Vars matching a given RT index with copies of TL expressions.
 *
 * Handles 'on retrieve to relation.attribute
 *			do instead retrieve (attribute = expression) w/qual'
 *
 * XXX Why is this not unified with apply_RIR_view()?
 */

typedef struct {
	List		   *rtable;
	List		   *targetlist;
	int				rt_index;
	int				attr_num;
	int			   *modified;
	int			   *badsql;
	int				sublevels_up;
} HandleRIRAttributeRule_context;

static Node *
HandleRIRAttributeRule_mutator(Node *node,
							   HandleRIRAttributeRule_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		int			this_varno = var->varno;
		int			this_varattno = var->varattno;
		int			this_varlevelsup = var->varlevelsup;

		if (this_varno == context->rt_index &&
			this_varattno == context->attr_num &&
			this_varlevelsup == context->sublevels_up)
		{
			if (var->vartype == 32)
			{					/* HACK: disallow SET variables */
				*context->modified = TRUE;
				*context->badsql = TRUE;
				return make_null(var->vartype);
			}
			else
			{
				char   *name_to_look_for;

				name_to_look_for = get_attname(getrelid(this_varno,
														context->rtable),
											   this_varattno);
				if (name_to_look_for)
				{
					Node	   *n;

					*context->modified = TRUE;
					n = FindMatchingTLEntry(context->targetlist,
											name_to_look_for);
					if (n == NULL)
						return make_null(var->vartype);
					/* Make a copy of the tlist item to return */
					n = copyObject(n);
					/* Adjust varlevelsup if tlist item is from higher query */
					if (this_varlevelsup > 0)
						IncrementVarSublevelsUp(n, this_varlevelsup, 0);
					return n;
				}
			}
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
			   HandleRIRAttributeRule_mutator, context);
		context->sublevels_up++;
		MUTATE(newnode->subselect, sublink->subselect, Node *,
			   HandleRIRAttributeRule_mutator, context);
		context->sublevels_up--;
		return (Node *) newnode;
	}
	if (IsA(node, Query))
	{
		Query  *query = (Query *) node;
		Query  *newnode;

		/*
		 * XXX original code for HandleRIRAttributeRule only recursed into
		 * qual field of subquery.  I'm assuming that was an oversight ...
		 */

		FLATCOPY(newnode, query, Query);
		MUTATE(newnode->targetList, query->targetList, List *,
			   HandleRIRAttributeRule_mutator, context);
		MUTATE(newnode->qual, query->qual, Node *,
			   HandleRIRAttributeRule_mutator, context);
		MUTATE(newnode->havingQual, query->havingQual, Node *,
			   HandleRIRAttributeRule_mutator, context);
		return (Node *) newnode;
	}
	return expression_tree_mutator(node, HandleRIRAttributeRule_mutator,
								   (void *) context);
}

void
HandleRIRAttributeRule(Query *parsetree,
					   List *rtable,
					   List *targetlist,
					   int rt_index,
					   int attr_num,
					   int *modified,
					   int *badsql)
{
	HandleRIRAttributeRule_context	context;

	context.rtable = rtable;
	context.targetlist = targetlist;
	context.rt_index = rt_index;
	context.attr_num = attr_num;
	context.modified = modified;
	context.badsql = badsql;
	context.sublevels_up = 0;

	parsetree->targetList = (List *) 
		HandleRIRAttributeRule_mutator((Node *) parsetree->targetList,
									   &context);
	parsetree->qual = HandleRIRAttributeRule_mutator(parsetree->qual,
													 &context);
	/* XXX original code did not fix havingQual ... oversight? */
	parsetree->havingQual = HandleRIRAttributeRule_mutator(parsetree->havingQual,
														   &context);
}
