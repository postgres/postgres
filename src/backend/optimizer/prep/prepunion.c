/*-------------------------------------------------------------------------
 *
 * prepunion.c--
 *	  Routines to plan inheritance, union, and version queries
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/prep/prepunion.c,v 1.16 1997/12/29 01:12:48 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include <sys/types.h>

#include "postgres.h"

#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"
#include "nodes/relation.h"

#include "parser/parsetree.h"
#include "parser/parse_clause.h"

#include "utils/elog.h"
#include "utils/lsyscache.h"

#include "optimizer/internal.h"
#include "optimizer/prep.h"
#include "optimizer/plancat.h"
#include "optimizer/planner.h"
#include "optimizer/planmain.h"

static List *plan_inherit_query(List *relids, Index rt_index,
				 RangeTblEntry *rt_entry, Query *parse,
				 List **union_rtentriesPtr);
static RangeTblEntry *new_rangetable_entry(Oid new_relid,
					 RangeTblEntry *old_entry);
static Query *subst_rangetable(Query *root, Index index,
				 RangeTblEntry *new_entry);
static void fix_parsetree_attnums(Index rt_index, Oid old_relid,
					  Oid new_relid, Query *parsetree);
static Append *make_append(List *unionplans, List *unionrts, Index rt_index,
			List *union_rt_entries, List *tlist);


/*
 * plan-union-queries--
 *
 *	  Plans the queries for a given UNION.
 *
 * Returns a list containing a list of plans and a list of rangetables
 */
Append	   *
plan_union_queries(Query *parse)
{
	List	*union_plans = NIL, *ulist, *unionall_queries, *union_rts,
			*last_union = NIL;
	bool	union_all_found = false, union_found = false,
			last_unionall_flag = false;
	
	/*
	 *	Do we need to split up our unions because we have UNION
	 *	and UNION ALL?
	 *
	 *	We are checking for the case of:
	 *		SELECT 1
	 *		UNION
	 *		SELECT 2
	 *		UNION
	 *		SELECT 3
	 *		UNION ALL
	 *		SELECT 4
	 *		UNION ALL
	 *		SELECT 5
	 *
	 *	where we have to do a DISTINCT on the output of the first three
	 *	queries, then add the rest.  If they have used UNION and
	 *	UNION ALL, we grab all queries up to the last UNION query,
	 *	make them their own UNION with the owner as the first query
	 *	in the list.  Then, we take the remaining queries, which is UNION
	 *	ALL, and add them to the list of union queries.
	 *
	 *	So the above query becomes:
	 *
	 *	Append Node
	 *	{
	 *		Sort and Unique
	 *		{
	 *			Append Node
	 *			{
	 *				SELECT 1				This is really a sub-UNION,
	 *				unionClause				We run a DISTINCT on these.
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
	 */

	foreach(ulist, parse->unionClause)
	{
		Query	*union_query = lfirst(ulist);

		if (union_query->unionall)
			union_all_found = true;
		else
		{
			union_found = true;
			last_union = ulist;
		}
		last_unionall_flag = union_query->unionall;
	}

	/* Is this a simple one */
	if (!union_all_found ||
		!union_found ||
		/*	A trailing UNION negates the affect of earlier UNION ALLs */
		!last_unionall_flag)
	{
		List *hold_unionClause = parse->unionClause;

		parse->unionClause = NIL;	/* prevent recursion */
		union_plans = lcons(planner(parse), NIL);
		union_rts = lcons(parse->rtable, NIL);

		foreach(ulist, hold_unionClause)
		{
			Query *union_query = lfirst(ulist);

			union_plans = lappend(union_plans, planner(union_query));
			union_rts = lappend(union_rts, union_query->rtable);
		}
	}
	else
	{
		/*
		 *	We have mixed unions and non-unions
		 *
		 *	We need to restructure this to put the UNIONs on their own
		 *	so we can do a DISTINCT.
		 */

		 /* save off everthing past the last UNION */
		unionall_queries = lnext(last_union);

		/* clip off the list to remove the trailing UNION ALLs */
		lnext(last_union) = NIL;

		/*
		 *	Recursion, but UNION only.
		 *	The last one is a UNION, so it will not come here in recursion,
		 */
		union_plans = lcons(planner(parse), NIL);
		union_rts = lcons(parse->rtable, NIL);

		/* Append the remainging UNION ALLs */
		foreach(ulist, unionall_queries)
		{
			Query	*unionall_query = lfirst(ulist);

			union_plans = lappend(union_plans, planner(unionall_query));
			union_rts = lappend(union_rts, unionall_query->rtable);
		}
	}
		
	/* We have already split UNION and UNION ALL and we made it consistent */
	if (!last_unionall_flag)
	{
		parse->uniqueFlag = "*";
		parse->sortClause = transformSortClause(NULL, NIL,
			((Plan *) lfirst(union_plans))->targetlist, "*");
	}
	else
	{
		/* needed so we don't take the flag from the first query */
		parse->uniqueFlag = NULL;
		parse->sortClause = NIL;
	}

	parse->havingQual = NULL;
	parse->qry_numAgg = 0;
	parse->qry_aggs = NULL;

	return (make_append(union_plans,
						union_rts,
						0,
						NULL,
						((Plan *) lfirst(union_plans))->targetlist));
}


/*
 * plan-inherit-queries--
 *
 *	  Plans the queries for a given parent relation.
 *
 * Returns a list containing a list of plans and a list of rangetable
 * entries to be inserted into an APPEND node.
 * XXX - what exactly does this mean, look for make_append
 */
Append	   *
plan_inherit_queries(Query *parse, Index rt_index)
{
	List	   *union_plans = NIL;

	List	   *rangetable = parse->rtable;
	RangeTblEntry *rt_entry = rt_fetch(rt_index, rangetable);
	List	   *union_rt_entries = NIL;
	List	   *union_relids = NIL;

	union_relids =
		find_all_inheritors(lconsi(rt_entry->relid,
								   NIL),
							NIL);
	/*
	 * Remove the flag for this relation, since we're about to handle it
	 * (do it before recursing!). XXX destructive parse tree change
	 */
	rt_fetch(rt_index, rangetable)->inh = false;

	union_plans = plan_inherit_query(union_relids, rt_index, rt_entry,
								   parse, &union_rt_entries);

	return (make_append(union_plans,
						NULL,
						rt_index,
						union_rt_entries,
						((Plan *) lfirst(union_plans))->targetlist));
}

/*
 * plan-inherit-query--
 *	  Returns a list of plans for 'relids' and a list of range table entries
 *	  in union_rtentries.
 */
static List *
plan_inherit_query(List *relids,
				 Index rt_index,
				 RangeTblEntry *rt_entry,
				 Query *root,
				 List **union_rtentriesPtr)
{
	List	   *i;
	List	   *union_plans = NIL;
	List	   *union_rtentries = NIL;

	foreach(i, relids)
	{
		int			relid = lfirsti(i);
		RangeTblEntry *new_rt_entry = new_rangetable_entry(relid,
														   rt_entry);
		Query	   *new_root = subst_rangetable(root,
												rt_index,
												new_rt_entry);

		/*
		 * reset the uniqueflag and sortclause in parse tree root, so that
		 * sorting will only be done once after append
		 */
		new_root->uniqueFlag = NULL;
		new_root->sortClause = NULL;
		new_root->groupClause = NULL;
		if (new_root->qry_numAgg != 0)
		{
			new_root->qry_numAgg = 0;
			pfree(new_root->qry_aggs);
			new_root->qry_aggs = NULL;
			del_agg_tlist_references(new_root->targetList);
		}
		fix_parsetree_attnums(rt_index,
							  rt_entry->relid,
							  relid,
							  new_root);

		union_plans = lappend(union_plans, planner(new_root));
		union_rtentries = lappend(union_rtentries, new_rt_entry);
	}

	*union_rtentriesPtr = union_rtentries;
	return (union_plans);
}

/*
 * find-all-inheritors -
 *		Returns a list of relids corresponding to relations that inherit
 *		attributes from any relations listed in either of the argument relid
 *		lists.
 */
List	   *
find_all_inheritors(List *unexamined_relids,
					List *examined_relids)
{
	List	   *new_inheritors = NIL;
	List	   *new_examined_relids = NIL;
	List	   *new_unexamined_relids = NIL;

	/*
	 * Find all relations which inherit from members of
	 * 'unexamined-relids' and store them in 'new-inheritors'.
	 */
	List	   *rels = NIL;
	List	   *newrels = NIL;

	foreach(rels, unexamined_relids)
	{
		newrels = (List *) LispUnioni(find_inheritance_children(lfirsti(rels)),
									  newrels);
	}
	new_inheritors = newrels;

	new_examined_relids = (List *) LispUnioni(examined_relids, unexamined_relids);
	new_unexamined_relids = set_differencei(new_inheritors,
											new_examined_relids);

	if (new_unexamined_relids == NULL)
	{
		return (new_examined_relids);
	}
	else
	{
		return (find_all_inheritors(new_unexamined_relids,
									new_examined_relids));
	}
}

/*
 * first-inherit-rt-entry -
 *		Given a rangetable, find the first rangetable entry that represents
 *		the appropriate special case.
 *
 *		Returns a rangetable index.,  Returns -1 if no matches
 */
int
first_inherit_rt_entry(List *rangetable)
{
	int			count = 0;
	List	   *temp = NIL;

	foreach(temp, rangetable)
	{
		RangeTblEntry *rt_entry = lfirst(temp);

		if (rt_entry->inh)
			return count + 1;
		count++;
	}

	return -1;
}

/*
 * new-rangetable-entry -
 *		Replaces the name and relid of 'old-entry' with the values for
 *		'new-relid'.
 *
 *		Returns a copy of 'old-entry' with the parameters substituted.
 */
static RangeTblEntry *
new_rangetable_entry(Oid new_relid, RangeTblEntry *old_entry)
{
	RangeTblEntry *new_entry = copyObject(old_entry);

	/* ??? someone tell me what the following is doing! - ay 11/94 */
	if (!strcmp(new_entry->refname, "*CURRENT*") ||
		!strcmp(new_entry->refname, "*NEW*"))
		new_entry->refname = get_rel_name(new_relid);
	else
		new_entry->relname = get_rel_name(new_relid);

	new_entry->relid = new_relid;
	return (new_entry);
}

/*
 * subst-rangetable--
 *	  Replaces the 'index'th rangetable entry in 'root' with 'new-entry'.
 *
 * Returns a new copy of 'root'.
 */
static Query *
subst_rangetable(Query *root, Index index, RangeTblEntry *new_entry)
{
	Query	   *new_root = copyObject(root);
	List	   *temp = NIL;
	int			i = 0;

	for (temp = new_root->rtable, i = 1; i < index; temp = lnext(temp), i++)
		;
	lfirst(temp) = new_entry;

	return (new_root);
}

static void
fix_parsetree_attnums_nodes(Index rt_index,
							Oid old_relid,
							Oid new_relid,
							Node *node)
{
	if (node == NULL)
		return;

	switch (nodeTag(node))
	{
		case T_TargetEntry:
			{
				TargetEntry *tle = (TargetEntry *) node;

				fix_parsetree_attnums_nodes(rt_index, old_relid, new_relid,
											tle->expr);
			}
			break;
		case T_Expr:
			{
				Expr	   *expr = (Expr *) node;

				fix_parsetree_attnums_nodes(rt_index, old_relid, new_relid,
											(Node *) expr->args);
			}
			break;
		case T_Var:
			{
				Var		   *var = (Var *) node;
				Oid			old_typeid,
							new_typeid;

				old_typeid = old_relid;
				new_typeid = new_relid;

				if (var->varno == rt_index && var->varattno != 0)
				{
					var->varattno =
						get_attnum(new_typeid,
								 get_attname(old_typeid, var->varattno));
				}
			}
			break;
		case T_List:
			{
				List	   *l;

				foreach(l, (List *) node)
				{
					fix_parsetree_attnums_nodes(rt_index, old_relid, new_relid,
												(Node *) lfirst(l));
				}
			}
			break;
		default:
			break;
	}
}

/*
 * fix-parsetree-attnums--
 *	  Replaces attribute numbers from the relation represented by
 *	  'old-relid' in 'parsetree' with the attribute numbers from
 *	  'new-relid'.
 *
 * Returns the destructively-modified parsetree.
 *
 */
static void
fix_parsetree_attnums(Index rt_index,
					  Oid old_relid,
					  Oid new_relid,
					  Query *parsetree)
{
	if (old_relid == new_relid)
		return;

	fix_parsetree_attnums_nodes(rt_index, old_relid, new_relid,
								(Node *) parsetree->targetList);
	fix_parsetree_attnums_nodes(rt_index, old_relid, new_relid,
								parsetree->qual);
}

static Append *
make_append(List *unionplans,
			List *unionrts,
			Index rt_index,
			List *union_rt_entries,
			List *tlist)
{
	Append	   *node = makeNode(Append);

	node->unionplans = unionplans;
	node->unionrts = unionrts;
	node->unionrelid = rt_index;
	node->unionrtentries = union_rt_entries;
	node->plan.cost = 0.0;
	node->plan.state = (EState *) NULL;
	node->plan.targetlist = tlist;
	node->plan.qual = NIL;
	node->plan.lefttree = (Plan *) NULL;
	node->plan.righttree = (Plan *) NULL;

	return (node);
}
