/*-------------------------------------------------------------------------
 *
 * planmain.c
 *	  Routines to plan a single query
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/planmain.c,v 1.37.2.1 1999/08/02 06:27:02 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"


#include "optimizer/clauses.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"


static Plan *subplanner(Query *root, List *flat_tlist, List *qual);
static Result *make_result(List *tlist, Node *resconstantqual, Plan *subplan);

/*
 * query_planner
 *	  Routine to create a query plan.  It does so by first creating a
 *	  subplan for the topmost level of attributes in the query.  Then,
 *	  it modifies all target list and qualifications to consider the next
 *	  level of nesting and creates a plan for this modified query by
 *	  recursively calling itself.  The two pieces are then merged together
 *	  by creating a result node that indicates which attributes should
 *	  be placed where and any relation level qualifications to be
 *	  satisfied.
 *
 *	  command-type is the query command, e.g., select, delete, etc.
 *	  tlist is the target list of the query
 *	  qual is the qualification of the query
 *
 *	  Returns a query plan.
 */
Plan *
query_planner(Query *root,
			  int command_type,
			  List *tlist,
			  List *qual)
{
	List	   *constant_qual = NIL;
	List	   *var_only_tlist;
	List	   *level_tlist;
	Plan	   *subplan;

	if (PlannerQueryLevel > 1)
	{
		/* should copy be made ? */
		tlist = (List *) SS_replace_correlation_vars((Node *) tlist);
		qual = (List *) SS_replace_correlation_vars((Node *) qual);
	}
	if (root->hasSubLinks)
		qual = (List *) SS_process_sublinks((Node *) qual);

	qual = cnfify((Expr *) qual, true);
#ifdef OPTIMIZER_DEBUG
	printf("After cnfify()\n");
	pprint(qual);
#endif

	/*
	 * Pull out any non-variable qualifications so these can be put in the
	 * topmost result node.
	 */
	qual = pull_constant_clauses(qual, &constant_qual);
	/*
	 * The opids for the variable qualifications will be fixed later, but
	 * someone seems to think that the constant quals need to be fixed here.
	 */
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
													NIL,
													root->resultRelation,
													(Plan *) NULL);

					if (constant_qual != NULL)
						return ((Plan *) make_result(tlist,
													 (Node *) constant_qual,
													 (Plan *) scan));
					else
						return (Plan *) scan;
				}
				break;
			default:
				return (Plan *) NULL;
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
		subplan = (Plan *) make_result(tlist,
									   (Node *) constant_qual,
									   subplan);

		/*
		 * Fix all varno's of the Result's node target list.
		 */
		set_tlist_references(subplan);

		return subplan;
	}

	/*
	 * fix up the flattened target list of the plan root node so that
	 * expressions are evaluated.  this forces expression evaluations that
	 * may involve expensive function calls to be delayed to the very last
	 * stage of query execution.  this could be bad. but it is joey's
	 * responsibility to optimally push these expressions down the plan
	 * tree.  -- Wei
	 *
	 * Note: formerly there was a test here to skip the flatten call if we
	 * expected union_planner to insert a Group or Agg node above our
	 * result. However, now union_planner tells us exactly what it wants
	 * returned, and we just do it.  Much cleaner.
	 */
	else
	{
		subplan->targetlist = flatten_tlist_vars(tlist,
												 subplan->targetlist);
		return subplan;
	}

#ifdef NOT_USED

	/*
	 * Destructively modify the query plan's targetlist to add fjoin lists
	 * to flatten functions that return sets of base types
	 */
	subplan->targetlist = generate_fjoin(subplan->targetlist);
#endif

}

/*
 * subplanner
 *
 *	 Subplanner creates an entire plan consisting of joins and scans
 *	 for processing a single level of attributes.
 *
 *	 flat_tlist is the flattened target list
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
	RelOptInfo *final_rel;

	/*
	 * Initialize the targetlist and qualification, adding entries to
	 * base_rel_list as relation references are found (e.g., in the
	 * qualification, the targetlist, etc.)
	 */
	root->base_rel_list = NIL;
	root->join_rel_list = NIL;

	make_var_only_tlist(root, flat_tlist);
	add_restrict_and_join_to_rels(root, qual);
	add_missing_vars_to_tlist(root, flat_tlist);

	set_joininfo_mergeable_hashable(root->base_rel_list);

	final_rel = make_one_rel(root, root->base_rel_list);

#ifdef NOT_USED					/* fix xfunc */

	/*
	 * Perform Predicate Migration on each path, to optimize and correctly
	 * assess the cost of each before choosing the cheapest one. -- JMH,
	 * 11/16/92
	 *
	 * Needn't do so if the top rel is pruneable: that means there's no
	 * expensive functions left to pull up.  -- JMH, 11/22/92
	 */
	if (XfuncMode != XFUNC_OFF && XfuncMode != XFUNC_NOPM &&
		XfuncMode != XFUNC_NOPULL && !final_rel->pruneable)
	{
		List	   *pathnode;

		foreach(pathnode, final_rel->pathlist)
		{
			if (xfunc_do_predmig((Path *) lfirst(pathnode)))
				set_cheapest(final_rel, final_rel->pathlist);
		}
	}
#endif

	/*
	 * Determine the cheapest path and create a subplan corresponding to
	 * it.
	 */
	if (final_rel)
		return create_plan((Path *) final_rel->cheapestpath);
	else
	{
		elog(NOTICE, "final relation is null");
		return create_plan((Path *) NULL);
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

	return node;
}
