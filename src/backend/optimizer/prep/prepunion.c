/*-------------------------------------------------------------------------
 *
 * prepunion.c
 *	  Routines to plan inheritance, union, and version queries
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/prep/prepunion.c,v 1.44 2000/02/15 03:37:26 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"

#include "optimizer/clauses.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"
#include "parser/parse_clause.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

typedef struct {
	Index		rt_index;
	int			sublevels_up;
	Oid			old_relid;
	Oid			new_relid;
} fix_parsetree_attnums_context;

static List *plan_inherit_query(Relids relids, Index rt_index,
				   RangeTblEntry *rt_entry, Query *parse, List *tlist,
				   List **union_rtentriesPtr);
static RangeTblEntry *new_rangetable_entry(Oid new_relid,
					 RangeTblEntry *old_entry);
static void fix_parsetree_attnums(Index rt_index, Oid old_relid,
					  Oid new_relid, Query *parsetree);
static bool fix_parsetree_attnums_walker (Node *node,
								fix_parsetree_attnums_context *context);
static Append *make_append(List *appendplans, List *unionrtables,
						   Index rt_index,
						   List *inheritrtable, List *tlist);


/*
 * plan_union_queries
 *
 *	  Plans the queries for a given UNION.
 *
 * Returns a list containing a list of plans and a list of rangetables
 */
Append *
plan_union_queries(Query *parse)
{
	List	   *union_plans = NIL,
			   *ulist,
			   *union_all_queries,
			   *union_rts,
			   *last_union = NIL,
			   *hold_sortClause = parse->sortClause;
	bool		union_all_found = false,
				union_found = false,
				last_union_all_flag = false;

	/*------------------------------------------------------------------
	 *
	 * Do we need to split up our unions because we have UNION and UNION
	 * ALL?
	 *
	 * We are checking for the case of: SELECT 1 UNION SELECT 2 UNION SELECT
	 * 3 UNION ALL SELECT 4 UNION ALL SELECT 5
	 *
	 * where we have to do a DISTINCT on the output of the first three
	 * queries, then add the rest.	If they have used UNION and UNION ALL,
	 * we grab all queries up to the last UNION query, make them their own
	 * UNION with the owner as the first query in the list.  Then, we take
	 * the remaining queries, which is UNION ALL, and add them to the list
	 * of union queries.
	 *
	 * So the above query becomes:
	 *
	 *	Append Node
	 *	{
	 *		Sort and Unique
	 *		{
	 *			Append Node
	 *			{
	 *				SELECT 1		This is really a sub-UNION.
	 *				unionClause		We run a DISTINCT on these.
	 *				{
	 *					SELECT 2
	 *					SELECT 3
	 *				}
	 *			}
	 *		}
	 *		SELECT 4
	 *		SELECT 5
	 *	}
	 *
	 *---------------------------------------------------------------------
	 */

	foreach(ulist, parse->unionClause)
	{
		Query	   *union_query = lfirst(ulist);

		if (union_query->unionall)
			union_all_found = true;
		else
		{
			union_found = true;
			last_union = ulist;
		}
		last_union_all_flag = union_query->unionall;
	}

	/* Is this a simple one */
	if (!union_all_found ||
		!union_found ||
	/* A trailing UNION negates the affect of earlier UNION ALLs */
		!last_union_all_flag)
	{
		List	   *hold_unionClause = parse->unionClause;

		/* we will do this later, so don't do it now */
		if (!union_all_found ||
			!last_union_all_flag)
		{
			parse->sortClause = NIL;
			parse->distinctClause = NIL;
		}

		parse->unionClause = NIL;		/* prevent recursion */
		union_plans = lcons(union_planner(parse), NIL);
		union_rts = lcons(parse->rtable, NIL);

		foreach(ulist, hold_unionClause)
		{
			Query	   *union_query = lfirst(ulist);

			union_plans = lappend(union_plans, union_planner(union_query));
			union_rts = lappend(union_rts, union_query->rtable);
		}
	}
	else
	{

		/*
		 * We have mixed unions and non-unions
		 *
		 * We need to restructure this to put the UNIONs on their own so we
		 * can do a DISTINCT.
		 */

		/* save off everthing past the last UNION */
		union_all_queries = lnext(last_union);

		/* clip off the list to remove the trailing UNION ALLs */
		lnext(last_union) = NIL;

		/*
		 * Recursion, but UNION only. The last one is a UNION, so it will
		 * not come here in recursion,
		 */
		union_plans = lcons(union_planner(parse), NIL);
		union_rts = lcons(parse->rtable, NIL);

		/* Append the remaining UNION ALLs */
		foreach(ulist, union_all_queries)
		{
			Query	   *union_all_query = lfirst(ulist);

			union_plans = lappend(union_plans, union_planner(union_all_query));
			union_rts = lappend(union_rts, union_all_query->rtable);
		}
	}

	/* We have already split UNION and UNION ALL and we made it consistent */
	if (!last_union_all_flag)
	{
		/* Need SELECT DISTINCT behavior to implement UNION.
		 * Put back the held sortClause, add any missing columns to the
		 * sort clause, and set distinctClause properly.
		 */
		List	   *slitem;

		parse->sortClause = addAllTargetsToSortList(hold_sortClause,
													parse->targetList);
		parse->distinctClause = NIL;
		foreach(slitem, parse->sortClause)
		{
			SortClause *scl = (SortClause *) lfirst(slitem);
			TargetEntry *tle = get_sortgroupclause_tle(scl, parse->targetList);

			if (! tle->resdom->resjunk)
				parse->distinctClause = lappend(parse->distinctClause,
												copyObject(scl));
		}
	}
	else
	{
		/* needed so we don't take SELECT DISTINCT from the first query */
		parse->distinctClause = NIL;
	}

	/* Make sure we don't try to apply the first query's grouping stuff
	 * to the Append node, either.  Basically we don't want union_planner
	 * to do anything when we return control, except add the top sort/unique
	 * nodes for DISTINCT processing if this wasn't UNION ALL, or the top
	 * sort node if it was UNION ALL with a user-provided sort clause.
	 */
	parse->groupClause = NULL;
	parse->havingQual = NULL;
	parse->hasAggs = false;

	return make_append(union_plans,
					   union_rts,
					   0,
					   NULL,
					   parse->targetList);
}


/*
 * plan_inherit_queries
 *
 *	  Plans the queries for an inheritance tree rooted at a parent relation.
 *
 * Inputs:
 *	parse = parent parse tree
 *	tlist = target list for inheritance subqueries (not same as parent's!)
 *	rt_index = rangetable index for current inheritance item
 *
 * Returns an APPEND node that forms the result of performing the given
 * query for each member relation of the inheritance group.
 *
 * If grouping, aggregation, or sorting is specified in the parent plan,
 * the subplans should not do any of those steps --- we must do those
 * operations just once above the APPEND node.  The given tlist has been
 * modified appropriately to remove group/aggregate expressions, but the
 * Query node still has the relevant fields set.  We remove them in the
 * copies used for subplans (see plan_inherit_query).
 *
 * NOTE: this can be invoked recursively if more than one inheritance wildcard
 * is present.  At each level of recursion, the first wildcard remaining in
 * the rangetable is expanded.
 */
Append *
plan_inherit_queries(Query *parse, List *tlist, Index rt_index)
{
	List	   *rangetable = parse->rtable;
	RangeTblEntry *rt_entry = rt_fetch(rt_index, rangetable);
	List	   *inheritrtable = NIL;
	List	   *union_relids;
	List	   *union_plans;

	/* Make a list of the target relid plus all its descendants */
	union_relids = find_all_inheritors(rt_entry->relid);

	/*
	 * Remove the flag for this relation, since we're about to handle it.
	 * XXX destructive change to parent parse tree, but necessary to prevent
	 * infinite recursion.
	 */
	rt_entry->inh = false;

	union_plans = plan_inherit_query(union_relids, rt_index, rt_entry,
									 parse, tlist, &inheritrtable);

	return make_append(union_plans,
					   NULL,
					   rt_index,
					   inheritrtable,
					   ((Plan *) lfirst(union_plans))->targetlist);
}

/*
 * plan_inherit_query
 *	  Returns a list of plans for 'relids', plus a list of range table entries
 *	  in *union_rtentriesPtr.
 */
static List *
plan_inherit_query(Relids relids,
				   Index rt_index,
				   RangeTblEntry *rt_entry,
				   Query *root,
				   List *tlist,
				   List **union_rtentriesPtr)
{
	List	   *union_plans = NIL;
	List	   *union_rtentries = NIL;
	List	   *save_tlist = root->targetList;
	List	   *i;

	/*
	 * Avoid making copies of the root's tlist, which we aren't going to
	 * use anyway (we are going to make copies of the passed tlist, instead).
	 */
	root->targetList = NIL;

	foreach(i, relids)
	{
		int			relid = lfirsti(i);
		/*
		 * Make a modifiable copy of the original query,
		 * and replace the target rangetable entry with a new one
		 * identifying this child table.
		 */
		Query	   *new_root = copyObject(root);
		RangeTblEntry *new_rt_entry = new_rangetable_entry(relid,
														   rt_entry);

		rt_store(rt_index, new_root->rtable, new_rt_entry);
		/*
		 * Insert (a modifiable copy of) the desired simplified tlist
		 * into the subquery
		 */
		new_root->targetList = copyObject(tlist);

		/*
		 * Clear the sorting and grouping qualifications in the subquery,
		 * so that sorting will only be done once after append
		 */
		new_root->distinctClause = NIL;
		new_root->sortClause = NIL;
		new_root->groupClause = NIL;
		new_root->havingQual = NULL;
		new_root->hasAggs = false; /* shouldn't be any left ... */

		/*
		 * Update attribute numbers in case child has different ordering
		 * of columns than parent (as can happen after ALTER TABLE).
		 *
		 * XXX This is a crock, and it doesn't really work.  It'd be better
		 * to fix ALTER TABLE to preserve consistency of attribute numbering.
		 */
		fix_parsetree_attnums(rt_index,
							  rt_entry->relid,
							  relid,
							  new_root);

		union_plans = lappend(union_plans, union_planner(new_root));
		union_rtentries = lappend(union_rtentries, new_rt_entry);
	}

	root->targetList = save_tlist;

	*union_rtentriesPtr = union_rtentries;
	return union_plans;
}

/*
 * find_all_inheritors -
 *		Returns an integer list of relids including the given rel plus
 *		all relations that inherit from it, directly or indirectly.
 */
List *
find_all_inheritors(Oid parentrel)
{
	List	   *examined_relids = NIL;
	List	   *unexamined_relids = lconsi(parentrel, NIL);

	/*
	 * While the queue of unexamined relids is nonempty, remove the
	 * first element, mark it examined, and find its direct descendants.
	 * NB: cannot use foreach(), since we modify the queue inside loop.
	 */
	while (unexamined_relids != NIL)
	{
		Oid		currentrel = lfirsti(unexamined_relids);
		List   *currentchildren;

		unexamined_relids = lnext(unexamined_relids);
		examined_relids = lappendi(examined_relids, currentrel);
		currentchildren = find_inheritance_children(currentrel);
		/*
		 * Add to the queue only those children not already seen.
		 * This could probably be simplified to a plain nconc,
		 * because our inheritance relationships should always be a
		 * strict tree, no?  Should never find any matches, ISTM...
		 */
		currentchildren = set_differencei(currentchildren, examined_relids);
		unexamined_relids = LispUnioni(unexamined_relids, currentchildren);
	}

	return examined_relids;
}

/*
 * first_inherit_rt_entry -
 *		Given a rangetable, find the first rangetable entry that represents
 *		an inheritance set.
 *
 *		Returns a rangetable index (1..n).
 *		Returns -1 if no matches
 */
int
first_inherit_rt_entry(List *rangetable)
{
	int			count = 0;
	List	   *temp;

	foreach(temp, rangetable)
	{
		RangeTblEntry *rt_entry = lfirst(temp);

		count++;
		if (rt_entry->inh)
			return count;
	}

	return -1;
}

/*
 * new_rangetable_entry -
 *		Replaces the name and relid of 'old_entry' with the values for
 *		'new_relid'.
 *
 *		Returns a copy of 'old_entry' with the parameters substituted.
 */
static RangeTblEntry *
new_rangetable_entry(Oid new_relid, RangeTblEntry *old_entry)
{
	RangeTblEntry *new_entry = copyObject(old_entry);

	/* ??? someone tell me what the following is doing! - ay 11/94 */
	if (!strcmp(new_entry->ref->relname, "*CURRENT*") ||
		!strcmp(new_entry->ref->relname, "*NEW*"))
		new_entry->ref->relname = get_rel_name(new_relid);
	else
		new_entry->relname = get_rel_name(new_relid);

	new_entry->relid = new_relid;
	return new_entry;
}

/*
 * fix_parsetree_attnums
 *	  Replaces attribute numbers from the relation represented by
 *	  'old_relid' in 'parsetree' with the attribute numbers from
 *	  'new_relid'.
 *
 * The parsetree is MODIFIED IN PLACE.  This is OK only because
 * plan_inherit_query made a copy of the tree for us to hack upon.
 */
static void
fix_parsetree_attnums(Index rt_index,
					  Oid old_relid,
					  Oid new_relid,
					  Query *parsetree)
{
	fix_parsetree_attnums_context context;

	if (old_relid == new_relid)
		return;					/* no work needed for parent rel itself */

	context.rt_index = rt_index;
	context.old_relid = old_relid;
	context.new_relid = new_relid;
	context.sublevels_up = 0;
	/*
	 * We must scan both the targetlist and qual, but we know the
	 * havingQual is empty, so we can ignore it.
	 */
	fix_parsetree_attnums_walker((Node *) parsetree->targetList, &context);
	fix_parsetree_attnums_walker((Node *) parsetree->qual, &context);
}

/*
 * Adjust varnos for child tables.  This routine makes it possible for
 * child tables to have different column positions for the "same" attribute
 * as a parent, which helps ALTER TABLE ADD COLUMN.  Unfortunately this isn't
 * nearly enough to make it work transparently; there are other places where
 * things fall down if children and parents don't have the same column numbers
 * for inherited attributes.  It'd be better to rip this code out and fix
 * ALTER TABLE...
 */
static bool
fix_parsetree_attnums_walker (Node *node,
							  fix_parsetree_attnums_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == context->sublevels_up &&
			var->varno == context->rt_index &&
			var->varattno > 0)
		{
			var->varattno = get_attnum(context->new_relid,
									   get_attname(context->old_relid,
												   var->varattno));
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

		if (fix_parsetree_attnums_walker((Node *) (sub->lefthand), context))
			return true;
		context->sublevels_up++;
		if (fix_parsetree_attnums_walker((Node *) (sub->subselect), context))
		{
			context->sublevels_up--;
			return true;
		}
		context->sublevels_up--;
		return false;
	}
	if (IsA(node, Query))
	{
		/* Reach here after recursing down into subselect above... */
		Query	   *qry = (Query *) node;

		if (fix_parsetree_attnums_walker((Node *) (qry->targetList), context))
			return true;
		if (fix_parsetree_attnums_walker((Node *) (qry->qual), context))
			return true;
		if (fix_parsetree_attnums_walker((Node *) (qry->havingQual), context))
			return true;
		return false;
	}
	return expression_tree_walker(node, fix_parsetree_attnums_walker,
								  (void *) context);
}

static Append *
make_append(List *appendplans,
			List *unionrtables,
			Index rt_index,
			List *inheritrtable,
			List *tlist)
{
	Append	   *node = makeNode(Append);
	List	   *subnode;

	node->appendplans = appendplans;
	node->unionrtables = unionrtables;
	node->inheritrelid = rt_index;
	node->inheritrtable = inheritrtable;
	node->plan.cost = 0;
	node->plan.plan_rows = 0;
	node->plan.plan_width = 0;
	foreach(subnode, appendplans)
	{
		Plan   *subplan = (Plan *) lfirst(subnode);

		node->plan.cost += subplan->cost;
		node->plan.plan_rows += subplan->plan_rows;
		if (node->plan.plan_width < subplan->plan_width)
			node->plan.plan_width = subplan->plan_width;
	}
	node->plan.state = (EState *) NULL;
	node->plan.targetlist = tlist;
	node->plan.qual = NIL;
	node->plan.lefttree = (Plan *) NULL;
	node->plan.righttree = (Plan *) NULL;

	return node;
}
