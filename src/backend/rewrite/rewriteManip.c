/*-------------------------------------------------------------------------
 *
 * rewriteManip.c
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteManip.c,v 1.56 2001/03/22 03:59:44 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "parser/parse_clause.h"
#include "rewrite/rewriteManip.h"
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

	/*
	 * If a Query is passed, examine it --- but we will not recurse into
	 * sub-Queries.
	 */
	if (node && IsA(node, Query))
		return query_tree_walker((Query *) node, checkExprHasAggs_walker,
								 NULL, false);
	else
		return checkExprHasAggs_walker(node, NULL);
}

static bool
checkExprHasAggs_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
		return true;			/* abort the tree traversal and return
								 * true */
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

	/*
	 * If a Query is passed, examine it --- but we will not recurse into
	 * sub-Queries.
	 */
	if (node && IsA(node, Query))
		return query_tree_walker((Query *) node, checkExprHasSubLink_walker,
								 NULL, false);
	else
		return checkExprHasSubLink_walker(node, NULL);
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
 * The varnoold fields are adjusted similarly.	Also, RangeTblRef nodes
 * in join trees and setOp trees are adjusted.
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
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node, OffsetVarNodes_walker,
								   (void *) context, true);
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
						  (void *) &context, true);
	}
	else
		OffsetVarNodes_walker(node, &context);
}

/*
 * ChangeVarNodes - adjust Var nodes for a specific change of RT index
 *
 * Find all Var nodes in the given tree belonging to a specific relation
 * (identified by sublevels_up and rt_index), and change their varno fields
 * to 'new_index'.	The varnoold fields are changed too.  Also, RangeTblRef
 * nodes in join trees and setOp trees are adjusted.
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
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node, ChangeVarNodes_walker,
								   (void *) context, true);
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
						  (void *) &context, true);
	}
	else
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
		return false;
	}
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->min_sublevels_up++;
		result = query_tree_walker((Query *) node,
								   IncrementVarSublevelsUp_walker,
								   (void *) context, true);
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
	 * if it's a Query, go straight to query_tree_walker to make sure that
	 * sublevels_up doesn't get incremented prematurely.
	 */
	if (node && IsA(node, Query))
		query_tree_walker((Query *) node, IncrementVarSublevelsUp_walker,
						  (void *) &context, true);
	else
		IncrementVarSublevelsUp_walker(node, &context);
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
	if (IsA(node, Query))
	{
		/* Recurse into subselects */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node, rangeTableEntry_used_walker,
								   (void *) context, true);
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
	 * if it's a Query, go straight to query_tree_walker to make sure that
	 * sublevels_up doesn't get incremented prematurely.
	 */
	if (node && IsA(node, Query))
		return query_tree_walker((Query *) node, rangeTableEntry_used_walker,
								 (void *) &context, true);
	else
		return rangeTableEntry_used_walker(node, &context);
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
								   (void *) context, true);
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
	 * if it's a Query, go straight to query_tree_walker to make sure that
	 * sublevels_up doesn't get incremented prematurely.
	 */
	if (node && IsA(node, Query))
		return query_tree_walker((Query *) node, attribute_used_walker,
								 (void *) &context, true);
	else
		return attribute_used_walker(node, &context);
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
		strcmp(rt_fetch(PRS2_OLD_VARNO, parsetree->rtable)->eref->relname,
			   "*OLD*") == 0 &&
		strcmp(rt_fetch(PRS2_NEW_VARNO, parsetree->rtable)->eref->relname,
			   "*NEW*") == 0)
		return parsetree;
	Assert(parsetree->jointree && IsA(parsetree->jointree, FromExpr));
	if (length(parsetree->jointree->fromlist) != 1)
		elog(ERROR, "getInsertSelectQuery: expected to find SELECT subquery");
	rtr = (RangeTblRef *) lfirst(parsetree->jointree->fromlist);
	Assert(IsA(rtr, RangeTblRef));
	selectrte = rt_fetch(rtr->rtindex, parsetree->rtable);
	selectquery = selectrte->subquery;
	if (!(selectquery && IsA(selectquery, Query) &&
		  selectquery->commandType == CMD_SELECT))
		elog(ERROR, "getInsertSelectQuery: expected to find SELECT subquery");
	if (length(selectquery->rtable) >= 2 &&
	 strcmp(rt_fetch(PRS2_OLD_VARNO, selectquery->rtable)->eref->relname,
			"*OLD*") == 0 &&
	 strcmp(rt_fetch(PRS2_NEW_VARNO, selectquery->rtable)->eref->relname,
			"*NEW*") == 0)
	{
		if (subquery_ptr)
			*subquery_ptr = &(selectrte->subquery);
		return selectquery;
	}
	elog(ERROR, "getInsertSelectQuery: can't find rule placeholders");
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
		 * Noplace to put the qual on a utility statement.
		 *
		 * For now, we expect utility stmt to be a NOTIFY, so give a specific
		 * error message for that case.
		 */
		if (parsetree->utilityStmt && IsA(parsetree->utilityStmt, NotifyStmt))
			elog(ERROR, "Conditional NOTIFY is not implemented");
		else
			elog(ERROR, "Conditional utility statements are not implemented");
	}

	/* INTERSECT want's the original, but we need to copy - Jan */
	copy = copyObject(qual);

	parsetree->jointree->quals = make_and_qual(parsetree->jointree->quals,
											   copy);

	/*
	 * Make sure query is marked correctly if added qual has sublinks or
	 * aggregates (not sure it can ever have aggs, but sublinks
	 * definitely).
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
	Node	   *copy;

	if (havingQual == NULL)
		return;

	if (parsetree->commandType == CMD_UTILITY)
	{

		/*
		 * Noplace to put the qual on a utility statement.
		 *
		 * For now, we expect utility stmt to be a NOTIFY, so give a specific
		 * error message for that case.
		 */
		if (parsetree->utilityStmt && IsA(parsetree->utilityStmt, NotifyStmt))
			elog(ERROR, "Conditional NOTIFY is not implemented");
		else
			elog(ERROR, "Conditional utility statements are not implemented");
	}

	/* INTERSECT want's the original, but we need to copy - Jan */
	copy = copyObject(havingQual);

	parsetree->havingQual = make_and_qual(parsetree->havingQual,
										  copy);

	/*
	 * Make sure query is marked correctly if added qual has sublinks or
	 * aggregates (not sure it can ever have aggs, but sublinks
	 * definitely).
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

#ifdef NOT_USED

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
		if (strcmp(e_attname, resname) == 0)
			return tle->expr;
	}
	return NULL;
}

#endif


/*
 * ResolveNew - replace Vars with corresponding items from a targetlist
 *
 * Vars matching target_varno and sublevels_up are replaced by the
 * entry with matching resno from targetlist, if there is one.
 * If not, we either change the unmatched Var's varno to update_varno
 * (when event == CMD_UPDATE) or replace it with a constant NULL.
 */

typedef struct
{
	int			target_varno;
	int			sublevels_up;
	List	   *targetlist;
	int			event;
	int			update_varno;
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
			Node	   *n = FindMatchingNew(context->targetlist,
											var->varattno);

			if (n == NULL)
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
					return (Node *) makeNullConst(var->vartype);
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
		SubLink    *sublink = (SubLink *) node;
		SubLink    *newnode;

		FLATCOPY(newnode, sublink, SubLink);
		MUTATE(newnode->lefthand, sublink->lefthand, List *,
			   ResolveNew_mutator, context);
		MUTATE(newnode->subselect, sublink->subselect, Node *,
			   ResolveNew_mutator, context);
		return (Node *) newnode;
	}
	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;
		Query	   *newnode;

		FLATCOPY(newnode, query, Query);
		context->sublevels_up++;
		query_tree_mutator(newnode, ResolveNew_mutator, context, true);
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

	/*
	 * Must be prepared to start with a Query or a bare expression tree;
	 * if it's a Query, go straight to query_tree_mutator to make sure
	 * that sublevels_up doesn't get incremented prematurely.
	 */
	if (node && IsA(node, Query))
	{
		Query	   *query = (Query *) node;
		Query	   *newnode;

		FLATCOPY(newnode, query, Query);
		query_tree_mutator(newnode, ResolveNew_mutator,
						   (void *) &context, true);
		return (Node *) newnode;
	}
	else
		return ResolveNew_mutator(node, &context);
}


#ifdef NOT_USED

/*
 * HandleRIRAttributeRule
 *	Replace Vars matching a given RT index with copies of TL expressions.
 *
 * Handles 'on retrieve to relation.attribute
 *			do instead retrieve (attribute = expression) w/qual'
 */

typedef struct
{
	List	   *rtable;
	List	   *targetlist;
	int			rt_index;
	int			attr_num;
	int		   *modified;
	int		   *badsql;
	int			sublevels_up;
}			HandleRIRAttributeRule_context;

static Node *
HandleRIRAttributeRule_mutator(Node *node,
							   HandleRIRAttributeRule_context * context)
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
				return (Node *) makeNullConst(var->vartype);
			}
			else
			{
				char	   *name_to_look_for;

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
						return (Node *) makeNullConst(var->vartype);
					/* Make a copy of the tlist item to return */
					n = copyObject(n);

					/*
					 * Adjust varlevelsup if tlist item is from higher
					 * query
					 */
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
		SubLink    *sublink = (SubLink *) node;
		SubLink    *newnode;

		FLATCOPY(newnode, sublink, SubLink);
		MUTATE(newnode->lefthand, sublink->lefthand, List *,
			   HandleRIRAttributeRule_mutator, context);
		MUTATE(newnode->subselect, sublink->subselect, Node *,
			   HandleRIRAttributeRule_mutator, context);
		return (Node *) newnode;
	}
	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;
		Query	   *newnode;

		FLATCOPY(newnode, query, Query);
		context->sublevels_up++;
		query_tree_mutator(newnode, HandleRIRAttributeRule_mutator,
						   context, true);
		context->sublevels_up--;
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
	HandleRIRAttributeRule_context context;

	context.rtable = rtable;
	context.targetlist = targetlist;
	context.rt_index = rt_index;
	context.attr_num = attr_num;
	context.modified = modified;
	context.badsql = badsql;
	context.sublevels_up = 0;

	query_tree_mutator(parsetree, HandleRIRAttributeRule_mutator,
					   (void *) &context, true);
}

#endif	 /* NOT_USED */
