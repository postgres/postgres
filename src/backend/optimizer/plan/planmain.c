/*-------------------------------------------------------------------------
 *
 * planmain.c--
 *	  Routines to plan a single query
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/planmain.c,v 1.15 1998/01/07 21:04:04 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"
#include "nodes/relation.h"
#include "nodes/makefuncs.h"

#include "optimizer/planmain.h"
#include "optimizer/internal.h"
#include "optimizer/paths.h"
#include "optimizer/clauses.h"
#include "optimizer/keys.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "optimizer/xfunc.h"
#include "optimizer/cost.h"

#include "tcop/dest.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "nodes/memnodes.h"
#include "utils/mcxt.h"
#include "utils/lsyscache.h"

static Plan *subplanner(Query *root, List *flat_tlist, List *qual);
static Result *make_result(List *tlist, Node *resconstantqual, Plan *subplan);

extern Plan *
make_groupPlan(List **tlist, bool tuplePerGroup,
			   List *groupClause, Plan *subplan);

/*
 * query_planner--
 *	  Routine to create a query plan.  It does so by first creating a
 *	  subplan for the topmost level of attributes in the query.  Then,
 *	  it modifies all target list and qualifications to consider the next
 *	  level of nesting and creates a plan for this modified query by
 *	  recursively calling itself.  The two pieces are then merged together
 *	  by creating a result node that indicates which attributes should
 *	  be placed where and any relation level qualifications to be
 *	  satisfied.
 *
 *	  command-type is the query command, e.g., retrieve, delete, etc.
 *	  tlist is the target list of the query
 *	  qual is the qualification of the query
 *
 *	  Returns a query plan.
 */
Plan	   *
query_planner(Query *root,
			  int command_type,
			  List *tlist,
			  List *qual)
{
	List	   *constant_qual = NIL;
	List	   *var_only_tlist = NIL;
	List	   *level_tlist = NIL;
	Plan	   *subplan = (Plan *) NULL;

	/*
	 * A command without a target list or qualification is an error,
	 * except for "delete foo".
	 */
	if (tlist == NIL && qual == NULL)
	{
		if (command_type == CMD_DELETE ||

		/*
		 * Total hack here. I don't know how to handle statements like
		 * notify in action bodies. Notify doesn't return anything but
		 * scans a system table.
		 */
			command_type == CMD_NOTIFY)
		{
			return ((Plan *) make_seqscan(NIL,
										  NIL,
										  root->resultRelation,
										  (Plan *) NULL));
		}
		else
			return ((Plan *) NULL);
	}

	/*
	 * Pull out any non-variable qualifications so these can be put in the
	 * topmost result node.  The opids for the remaining qualifications
	 * will be changed to regprocs later.
	 */
	qual = pull_constant_clauses(qual, &constant_qual);
	fix_opids(constant_qual);

	/*
	 * Create a target list that consists solely of (resdom var) target
	 * list entries, i.e., contains no arbitrary expressions.
	 */
	var_only_tlist = flatten_tlist(tlist);
	if (var_only_tlist)
		level_tlist = var_only_tlist;
	else
		/* from old code. the logic is beyond me. - ay 2/95 */
		level_tlist = tlist;

	/*
	 * A query may have a non-variable target list and a non-variable
	 * qualification only under certain conditions: - the query creates
	 * all-new tuples, or - the query is a replace (a scan must still be
	 * done in this case).
	 */
	if (var_only_tlist == NULL && qual == NULL)
	{

		switch (command_type)
		{
			case CMD_SELECT:
			case CMD_INSERT:
				return ((Plan *) make_result(tlist,
											 (Node *) constant_qual,
											 (Plan *) NULL));
				break;

			case CMD_DELETE:
			case CMD_UPDATE:
				{
					SeqScan    *scan = make_seqscan(tlist,
													(List *) NULL,
													root->resultRelation,
													(Plan *) NULL);

					if (constant_qual != NULL)
					{
						return ((Plan *) make_result(tlist,
												  (Node *) constant_qual,
													 (Plan *) scan));
					}
					else
					{
						return ((Plan *) scan);
					}
				}
				break;

			default:
				return ((Plan *) NULL);
		}
	}

	/*
	 * Find the subplan (access path) and destructively modify the target
	 * list of the newly created subplan to contain the appropriate join
	 * references.
	 */
	subplan = subplanner(root, level_tlist, qual);

	set_tlist_references(subplan);

	/*
	 * Build a result node linking the plan if we have constant quals
	 */
	if (constant_qual)
	{
		Plan	   *plan;

		plan = (Plan *) make_result(tlist,
									(Node *) constant_qual,
									subplan);

		/*
		 * Change all varno's of the Result's node target list.
		 */
		set_result_tlist_references((Result *) plan);

		return (plan);
	}

	/*
	 * fix up the flattened target list of the plan root node so that
	 * expressions are evaluated.  this forces expression evaluations that
	 * may involve expensive function calls to be delayed to the very last
	 * stage of query execution.  this could be bad. but it is joey's
	 * responsibility to optimally push these expressions down the plan
	 * tree.  -- Wei
	 *
	 * But now nothing to do if there are GroupBy and/or Aggregates: 1.
	 * make_groupPlan fixes tlist; 2. flatten_tlist_vars does nothing with
	 * aggregates fixing only other entries (i.e. - GroupBy-ed and so
	 * fixed by make_groupPlan).	 - vadim 04/05/97
	 */
	if (root->groupClause == NULL && root->qry_aggs == NULL)
	{
		subplan->targetlist = flatten_tlist_vars(tlist,
												 subplan->targetlist);
	}

#ifdef NOT_USED
	/*
	 * Destructively modify the query plan's targetlist to add fjoin lists
	 * to flatten functions that return sets of base types
	 */
	subplan->targetlist = generate_fjoin(subplan->targetlist);
#endif

	return (subplan);
}

/*
 * subplanner
 *
 *	 Subplanner creates an entire plan consisting of joins and scans
 *	 for processing a single level of attributes.
 *
 *	 flat-tlist is the flattened target list
 *	 qual is the qualification to be satisfied
 *
 *	 Returns a subplan.
 *
 */
static Plan *
subplanner(Query *root,
		   List *flat_tlist,
		   List *qual)
{
	Rel		   *final_relation;
	List	   *final_relation_list;

	/*
	 * Initialize the targetlist and qualification, adding entries to
	 * *query-relation-list* as relation references are found (e.g., in
	 * the qualification, the targetlist, etc.)
	 */
	root->base_relation_list_ = NIL;
	root->join_relation_list_ = NIL;
	initialize_base_rels_list(root, flat_tlist);
	initialize_base_rels_jinfo(root, qual);
	add_missing_vars_to_base_rels(root, flat_tlist);

	/*
	 * Find all possible scan and join paths. Mark all the clauses and
	 * relations that can be processed using special join methods, then do
	 * the exhaustive path search.
	 */
	initialize_join_clause_info(root->base_relation_list_);
	final_relation_list = find_paths(root,
									 root->base_relation_list_);

	if (final_relation_list)
		final_relation = (Rel *) lfirst(final_relation_list);
	else
		final_relation = (Rel *) NIL;

#if 0							/* fix xfunc */

	/*
	 * Perform Predicate Migration on each path, to optimize and correctly
	 * assess the cost of each before choosing the cheapest one. -- JMH,
	 * 11/16/92
	 *
	 * Needn't do so if the top rel is pruneable: that means there's no
	 * expensive functions left to pull up.  -- JMH, 11/22/92
	 */
	if (XfuncMode != XFUNC_OFF && XfuncMode != XFUNC_NOPM &&
		XfuncMode != XFUNC_NOPULL && !final_relation->pruneable)
	{
		List	   *pathnode;

		foreach(pathnode, final_relation->pathlist)
		{
			if (xfunc_do_predmig((Path *) lfirst(pathnode)))
				set_cheapest(final_relation, final_relation->pathlist);
		}
	}
#endif

	/*
	 * Determine the cheapest path and create a subplan corresponding to
	 * it.
	 */
	if (final_relation)
	{
		return (create_plan((Path *) final_relation->cheapestpath));
	}
	else
	{
		elog(NOTICE, "final relation is nil");
		return (create_plan((Path *) NULL));
	}

}

/*****************************************************************************
 *
 *****************************************************************************/

static Result *
make_result(List *tlist,
			Node *resconstantqual,
			Plan *subplan)
{
	Result	   *node = makeNode(Result);
	Plan	   *plan = &node->plan;

#ifdef NOT_USED
	tlist = generate_fjoin(tlist);
#endif
    plan->cost = (subplan ? subplan->cost : 0);
	plan->state = (EState *) NULL;
	plan->targetlist = tlist;
	plan->lefttree = subplan;
	plan->righttree = NULL;
	node->resconstantqual = resconstantqual;
	node->resstate = NULL;

	return (node);
}

/*****************************************************************************
 *
 *****************************************************************************/

Plan *
make_groupPlan(List **tlist,
			   bool tuplePerGroup,
			   List *groupClause,
			   Plan *subplan)
{
	List	   *sort_tlist;
	List	   *sl,
			   *gl;
	List	   *glc = listCopy(groupClause);
	List	   *otles = NIL;	/* list of removed non-GroupBy entries */
	List	   *otlvars = NIL;	/* list of var in them */
	int			otlvcnt;
	Sort	   *sortplan;
	Group	   *grpplan;
	int			numCols;
	AttrNumber *grpColIdx;
	int			last_resno = 1;

	numCols = length(groupClause);
	grpColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);

	sort_tlist = new_unsorted_tlist(*tlist);	/* it's copy */

	/*
	 * Make template TL for subplan, Sort & Group: 1. If there are
	 * aggregates (tuplePerGroup is true) then take away non-GroupBy
	 * entries and re-set resno-s accordantly. 2. Make grpColIdx
	 *
	 * Note: we assume that TLEs in *tlist are ordered in accordance with
	 * their resdom->resno.
	 */
	foreach(sl, sort_tlist)
	{
		Resdom		   *resdom = NULL;
		TargetEntry	   *te = (TargetEntry *) lfirst(sl);
		int				keyno = 0;

		foreach(gl, groupClause)
		{
			GroupClause *grpcl = (GroupClause *) lfirst(gl);

			keyno++;
			if (grpcl->entry->resdom->resno == te->resdom->resno)
			{

				resdom = te->resdom;
				resdom->reskey = keyno;
				resdom->reskeyop = get_opcode(grpcl->grpOpoid);
				resdom->resno = last_resno;		/* re-set */
				grpColIdx[keyno - 1] = last_resno++;
				glc = lremove(lfirst(gl), glc); /* TLE found for it */
				break;
			}
		}

		/*
		 * Non-GroupBy entry: remove it from Group/Sort TL if there are
		 * aggregates in query - it will be evaluated by Aggregate plan
		 */
		if (resdom == NULL)
		{
			if (tuplePerGroup)
			{
				otlvars = nconc(otlvars, pull_var_clause(te->expr));
				otles = lcons(te, otles);
				sort_tlist = lremove(te, sort_tlist);
			}
			else
				te->resdom->resno = last_resno++;
		}
	}

	if (length(glc) != 0)
	{
		elog(ERROR, "group attribute disappeared from target list");
	}

	/*
	 * If non-GroupBy entries were removed from TL - we are to add Vars
	 * for them to the end of TL if there are no such Vars in TL already.
	 */

	otlvcnt = length(otlvars);
	foreach(gl, otlvars)
	{
		Var		   *v = (Var *) lfirst(gl);

		if (tlist_member(v, sort_tlist) == NULL)
		{
			sort_tlist = lappend(sort_tlist,
								 create_tl_element(v, last_resno));
			last_resno++;
		}
		else
/* already in TL */
			otlvcnt--;
	}
	/* Now otlvcnt is number of Vars added in TL for non-GroupBy entries */

	/* Make TL for subplan: substitute Vars from subplan TL into new TL */
	sl = flatten_tlist_vars(sort_tlist, subplan->targetlist);

	subplan->targetlist = new_unsorted_tlist(sl);		/* there */

	/*
	 * Make Sort/Group TL : 1. make Var nodes (with varno = 1 and varnoold
	 * = -1) for all functions, 'couse they will be evaluated by subplan;
	 * 2. for real Vars: set varno = 1 and varattno to its resno in
	 * subplan
	 */
	foreach(sl, sort_tlist)
	{
		TargetEntry *te = (TargetEntry *) lfirst(sl);
		Resdom	   *resdom = te->resdom;
		Node	   *expr = te->expr;

		if (IsA(expr, Var))
		{
#if 0							/* subplanVar->resdom->resno expected to
								 * be = te->resdom->resno */
			TargetEntry *subplanVar;

			subplanVar = match_varid((Var *) expr, subplan->targetlist);
			((Var *) expr)->varattno = subplanVar->resdom->resno;
#endif
			((Var *) expr)->varattno = te->resdom->resno;
			((Var *) expr)->varno = 1;
		}
		else
			te->expr = (Node *) makeVar(1, resdom->resno,
										resdom->restype,
										-1, resdom->resno);
	}

	sortplan = make_sort(sort_tlist,
						 _TEMP_RELATION_ID_,
						 subplan,
						 numCols);
	sortplan->plan.cost = subplan->cost;		/* XXX assume no cost */

	/*
	 * make the Group node
	 */
	sort_tlist = copyObject(sort_tlist);
	grpplan = make_group(sort_tlist, tuplePerGroup, numCols,
						 grpColIdx, sortplan);

	/*
	 * Make TL for parent: "restore" non-GroupBy entries (if they were
	 * removed) and set resno-s of others accordantly.
	 */
	sl = sort_tlist;
	sort_tlist = NIL;			/* to be new parent TL */
	foreach(gl, *tlist)
	{
		List	   *temp = NIL;
		TargetEntry *te = (TargetEntry *) lfirst(gl);

		foreach(temp, otles)	/* Is it removed non-GroupBy entry ? */
		{
			TargetEntry *ote = (TargetEntry *) lfirst(temp);

			if (ote->resdom->resno == te->resdom->resno)
			{
				otles = lremove(ote, otles);
				break;
			}
		}
		if (temp == NIL)		/* It's "our" TLE - we're to return */
		{						/* it from Sort/Group plans */
			TargetEntry *my = (TargetEntry *) lfirst(sl);		/* get it */

			sl = sl->next;		/* prepare for the next "our" */
			my = copyObject(my);
			my->resdom->resno = te->resdom->resno;		/* order of parent TL */
			sort_tlist = lappend(sort_tlist, my);
			continue;
		}
		/* else - it's TLE of an non-GroupBy entry */
		sort_tlist = lappend(sort_tlist, copyObject(te));
	}

	/*
	 * Pure non-GroupBy entries Vars were at the end of Group' TL. They
	 * shouldn't appear in parent TL, all others shouldn't disappear.
	 */
	Assert(otlvcnt == length(sl));
	Assert(length(otles) == 0);

	*tlist = sort_tlist;

	return (Plan *) grpplan;
}
