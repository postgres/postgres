/*-------------------------------------------------------------------------
 *
 * setrefs.c
 *	  Routines to change varno/attno entries to contain references
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/setrefs.c,v 1.55 1999/08/18 04:15:16 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"

typedef struct {
	List	   *outer_tlist;
	List	   *inner_tlist;
} replace_joinvar_refs_context;

typedef struct {
	Index		subvarno;
	List	   *subplanTargetList;
} replace_vars_with_subplan_refs_context;

typedef struct {
	List	   *groupClause;
	List	   *targetList;
} check_having_for_ungrouped_vars_context;

static void set_join_tlist_references(Join *join);
static void set_nonamescan_tlist_references(SeqScan *nonamescan);
static void set_noname_tlist_references(Noname *noname);
static Node *replace_joinvar_refs(Node *clause,
								  List *outer_tlist,
								  List *inner_tlist);
static Node *replace_joinvar_refs_mutator(Node *node,
					replace_joinvar_refs_context *context);
static List *tlist_noname_references(Oid nonameid, List *tlist);
static void set_result_tlist_references(Result *resultNode);
static void replace_vars_with_subplan_refs(Node *clause,
										   Index subvarno,
										   List *subplanTargetList);
static bool replace_vars_with_subplan_refs_walker(Node *node,
					replace_vars_with_subplan_refs_context *context);
static List *pull_agg_clause(Node *clause);
static bool pull_agg_clause_walker(Node *node, List **listptr);
static bool check_having_for_ungrouped_vars_walker(Node *node,
					check_having_for_ungrouped_vars_context *context);

/*****************************************************************************
 *
 *		SUBPLAN REFERENCES
 *
 *****************************************************************************/

/*
 * set_tlist_references
 *	  Modifies the target list of nodes in a plan to reference target lists
 *	  at lower levels.
 *
 * 'plan' is the plan whose target list and children's target lists will
 *		be modified
 *
 * Returns nothing of interest, but modifies internal fields of nodes.
 *
 */
void
set_tlist_references(Plan *plan)
{
	if (plan == NULL)
		return;

	if (IsA_Join(plan))
		set_join_tlist_references((Join *) plan);
	else if (IsA(plan, SeqScan) && plan->lefttree &&
			 IsA_Noname(plan->lefttree))
		set_nonamescan_tlist_references((SeqScan *) plan);
	else if (IsA_Noname(plan))
		set_noname_tlist_references((Noname *) plan);
	else if (IsA(plan, Result))
		set_result_tlist_references((Result *) plan);
	else if (IsA(plan, Hash))
		set_tlist_references(plan->lefttree);
}

/*
 * set_join_tlist_references
 *	  Modifies the target list of a join node by setting the varnos and
 *	  varattnos to reference the target list of the outer and inner join
 *	  relations.
 *
 *	  Creates a target list for a join node to contain references by setting
 *	  varno values to OUTER or INNER and setting attno values to the
 *	  result domain number of either the corresponding outer or inner join
 *	  tuple.
 *
 * 'join' is a join plan node
 *
 * Returns nothing of interest, but modifies internal fields of nodes.
 *
 */
static void
set_join_tlist_references(Join *join)
{
	Plan	   *outer = join->lefttree;
	Plan	   *inner = join->righttree;
	List	   *outer_tlist = ((outer == NULL) ? NIL : outer->targetlist);
	List	   *inner_tlist = ((inner == NULL) ? NIL : inner->targetlist);
	List	   *new_join_targetlist = NIL;
	List	   *qptlist = join->targetlist;
	List	   *entry;

	foreach(entry, qptlist)
	{
		TargetEntry *xtl = (TargetEntry *) lfirst(entry);
		Node	   *joinexpr = replace_joinvar_refs(xtl->expr,
													outer_tlist,
													inner_tlist);

		new_join_targetlist = lappend(new_join_targetlist,
									  makeTargetEntry(xtl->resdom, joinexpr));
	}
	join->targetlist = new_join_targetlist;

	set_tlist_references(outer);
	set_tlist_references(inner);
}

/*
 * set_nonamescan_tlist_references
 *	  Modifies the target list of a node that scans a noname relation (i.e., a
 *	  sort or materialize node) so that the varnos refer to the child noname.
 *
 * 'nonamescan' is a seqscan node
 *
 * Returns nothing of interest, but modifies internal fields of nodes.
 *
 */
static void
set_nonamescan_tlist_references(SeqScan *nonamescan)
{
	Noname	   *noname = (Noname *) nonamescan->plan.lefttree;

	nonamescan->plan.targetlist = tlist_noname_references(noname->nonameid,
									  nonamescan->plan.targetlist);
	/* since we know child is a Noname, skip recursion through
	 * set_tlist_references() and just get the job done
	 */
	set_noname_tlist_references(noname);
}

/*
 * set_noname_tlist_references
 *	  The noname's vars are made consistent with (actually, identical to) the
 *	  modified version of the target list of the node from which noname node
 *	  receives its tuples.
 *
 * 'noname' is a noname (e.g., sort, materialize) plan node
 *
 * Returns nothing of interest, but modifies internal fields of nodes.
 *
 */
static void
set_noname_tlist_references(Noname *noname)
{
	Plan	   *source = noname->plan.lefttree;

	if (source != NULL)
	{
		set_tlist_references(source);
		noname->plan.targetlist = copy_vars(noname->plan.targetlist,
											source->targetlist);
	}
	else
		elog(ERROR, "calling set_noname_tlist_references with empty lefttree");
}

/*
 * join_references
 *	   Creates a new set of join clauses by changing the varno/varattno
 *	   values of variables in the clauses to reference target list values
 *	   from the outer and inner join relation target lists.
 *	   This is just an external interface for replace_joinvar_refs.
 *
 * 'clauses' is the list of join clauses
 * 'outer_tlist' is the target list of the outer join relation
 * 'inner_tlist' is the target list of the inner join relation
 *
 * Returns the new join clauses.  The original clause structure is
 * not modified.
 *
 */
List *
join_references(List *clauses,
				List *outer_tlist,
				List *inner_tlist)
{
	return (List *) replace_joinvar_refs((Node *) clauses,
										 outer_tlist,
										 inner_tlist);
}

/*
 * replace_joinvar_refs
 *
 *	  Replaces all variables within a join clause with a new var node
 *	  whose varno/varattno fields contain a reference to a target list
 *	  element from either the outer or inner join relation.
 *
 *	  Returns a suitably modified copy of the join clause;
 *	  the original is not modified (and must not be!)
 *
 *	  Side effect: also runs fix_opids on the modified join clause.
 *	  Really ought to make that happen in a uniform, consistent place...
 *
 * 'clause' is the join clause
 * 'outer_tlist' is the target list of the outer join relation
 * 'inner_tlist' is the target list of the inner join relation
 */
static Node *
replace_joinvar_refs(Node *clause,
					 List *outer_tlist,
					 List *inner_tlist)
{
	replace_joinvar_refs_context context;

	context.outer_tlist = outer_tlist;
	context.inner_tlist = inner_tlist;
	return (Node *) fix_opids((List *)
							  replace_joinvar_refs_mutator(clause, &context));
}

static Node *
replace_joinvar_refs_mutator(Node *node,
							 replace_joinvar_refs_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		Resdom	   *resdom = tlist_member(var, context->outer_tlist);

		if (resdom != NULL && IsA(resdom, Resdom))
			return (Node *) makeVar(OUTER,
									resdom->resno,
									var->vartype,
									var->vartypmod,
									0,
									var->varnoold,
									var->varoattno);
		resdom = tlist_member(var, context->inner_tlist);
		if (resdom != NULL && IsA(resdom, Resdom))
			return (Node *) makeVar(INNER,
									resdom->resno,
									var->vartype,
									var->vartypmod,
									0,
									var->varnoold,
									var->varoattno);
		/* Var not in either tlist, return an unmodified copy. */
		return copyObject(node);
	}
	return expression_tree_mutator(node,
								   replace_joinvar_refs_mutator,
								   (void *) context);
}

/*
 * tlist_noname_references
 *	  Creates a new target list for a node that scans a noname relation,
 *	  setting the varnos to the id of the noname relation and setting varids
 *	  if necessary (varids are only needed if this is a targetlist internal
 *	  to the tree, in which case the targetlist entry always contains a var
 *	  node, so we can just copy it from the noname).
 *
 * 'nonameid' is the id of the noname relation
 * 'tlist' is the target list to be modified
 *
 * Returns new target list
 *
 */
static List *
tlist_noname_references(Oid nonameid,
						List *tlist)
{
	List	   *t_list = NIL;
	List	   *entry;

	foreach(entry, tlist)
	{
		TargetEntry *xtl = lfirst(entry);
		AttrNumber	oattno;
		TargetEntry *noname;

		if (IsA(get_expr(xtl), Var))
			oattno = ((Var *) xtl->expr)->varoattno;
		else
			oattno = 0;

		noname = makeTargetEntry(xtl->resdom,
								 (Node *) makeVar(nonameid,
												  xtl->resdom->resno,
												  xtl->resdom->restype,
												  xtl->resdom->restypmod,
												  0,
												  nonameid,
												  oattno));

		t_list = lappend(t_list, noname);
	}
	return t_list;
}

/*---------------------------------------------------------
 *
 * set_result_tlist_references
 *
 * Change the target list of a Result node, so that it correctly
 * addresses the tuples returned by its left tree subplan.
 *
 * NOTE:
 *	1) we ignore the right tree! (in the current implementation
 *	   it is always nil)
 *	2) this routine will probably *NOT* work with nested dot
 *	   fields....
 */
static void
set_result_tlist_references(Result *resultNode)
{
	Plan	   *subplan;
	List	   *resultTargetList;
	List	   *subplanTargetList;

	resultTargetList = ((Plan *) resultNode)->targetlist;

	/*
	 * NOTE: we only consider the left tree subplan. This is usually a seq
	 * scan.
	 */
	subplan = ((Plan *) resultNode)->lefttree;
	if (subplan != NULL)
		subplanTargetList = subplan->targetlist;
	else
		subplanTargetList = NIL;

	replace_tlist_with_subplan_refs(resultTargetList,
									(Index) OUTER,
									subplanTargetList);
}

/*---------------------------------------------------------
 *
 * replace_tlist_with_subplan_refs
 *
 * Applies replace_vars_with_subplan_refs() to each entry of a targetlist.
 */
void
replace_tlist_with_subplan_refs(List *tlist,
								Index subvarno,
								List *subplanTargetList)
{
	List	   *t;

	foreach(t, tlist)
	{
		TargetEntry *entry = (TargetEntry *) lfirst(t);

		replace_vars_with_subplan_refs((Node *) get_expr(entry),
									   subvarno, subplanTargetList);
	}
}

/*---------------------------------------------------------
 *
 * replace_vars_with_subplan_refs
 *
 * This routine modifies (destructively!) an expression tree so that all
 * Var nodes reference target nodes of a subplan.  It is used to fix up
 * target expressions of upper-level plan nodes.
 *
 * 'clause': the tree to be fixed
 * 'subvarno': varno to be assigned to all Vars
 * 'subplanTargetList': target list for subplan
 *
 * Afterwards, all Var nodes have varno = subvarno, varattno = resno
 * of corresponding subplan target.
 */
static void
replace_vars_with_subplan_refs(Node *clause,
							   Index subvarno,
							   List *subplanTargetList)
{
	replace_vars_with_subplan_refs_context context;

	context.subvarno = subvarno;
	context.subplanTargetList = subplanTargetList;
	replace_vars_with_subplan_refs_walker(clause, &context);
}

static bool
replace_vars_with_subplan_refs_walker(Node *node,
							 replace_vars_with_subplan_refs_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		/*
		 * It could be that this varnode has been created by make_groupplan
		 * and is already set up to reference the subplan target list. We
		 * recognize that case by varno = 1, varnoold = -1, varattno =
		 * varoattno, and varlevelsup = 0.	(Probably ought to have an
		 * explicit flag, but this should do for now.)
		 */
		Var		   *var = (Var *) node;
		TargetEntry *subplanVar;

		if (var->varno == (Index) 1 &&
			var->varnoold == ((Index) -1) &&
			var->varattno == var->varoattno &&
			var->varlevelsup == 0)
			return false;		/* OK to leave it alone */

		/* Otherwise it had better be in the subplan list. */
		subplanVar = match_varid(var, context->subplanTargetList);
		if (!subplanVar)
			elog(ERROR, "replace_vars_with_subplan_refs: variable not in target list");

		/*
		 * Change the varno & varattno fields of the var node.
		 */
		var->varno = context->subvarno;
		var->varattno = subplanVar->resdom->resno;
		return false;
	}
	return expression_tree_walker(node,
								  replace_vars_with_subplan_refs_walker,
								  (void *) context);
}

/*****************************************************************************
 *
 *****************************************************************************/

/*---------------------------------------------------------
 *
 * set_agg_tlist_references -
 *	  This routine has several responsibilities:
 *	* Update the target list of an Agg node so that it points to
 *	  the tuples returned by its left tree subplan.
 *	* If there is a qual list (from a HAVING clause), similarly update
 *	  vars in it to point to the subplan target list.
 *	* Generate the aggNode->aggs list of Aggref nodes contained in the Agg.
 *
 * The return value is TRUE if all qual clauses include Aggrefs, or FALSE
 * if any do not (caller may choose to raise an error condition).
 */
bool
set_agg_tlist_references(Agg *aggNode)
{
	List	   *subplanTargetList;
	List	   *tl;
	List	   *ql;
	bool		all_quals_ok;

	subplanTargetList = aggNode->plan.lefttree->targetlist;
	aggNode->aggs = NIL;

	foreach(tl, aggNode->plan.targetlist)
	{
		TargetEntry *tle = lfirst(tl);

		replace_vars_with_subplan_refs(tle->expr,
									   (Index) 0,
									   subplanTargetList);
		aggNode->aggs = nconc(pull_agg_clause(tle->expr), aggNode->aggs);
	}

	all_quals_ok = true;
	foreach(ql, aggNode->plan.qual)
	{
		Node	   *qual = lfirst(ql);
		List	   *qualaggs;

		replace_vars_with_subplan_refs(qual,
									   (Index) 0,
									   subplanTargetList);
		qualaggs = pull_agg_clause(qual);
		if (qualaggs == NIL)
			all_quals_ok = false;		/* this qual clause has no agg
										 * functions! */
		else
			aggNode->aggs = nconc(qualaggs, aggNode->aggs);
	}

	return all_quals_ok;
}

/*
 * pull_agg_clause
 *	  Recursively pulls all Aggref nodes from an expression clause.
 *
 *	  Returns list of Aggref nodes found.  Note the nodes themselves are not
 *	  copied, only referenced.
 */
static List *
pull_agg_clause(Node *clause)
{
	List	   *result = NIL;

	pull_agg_clause_walker(clause, &result);
	return result;
}

static bool
pull_agg_clause_walker(Node *node, List **listptr)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		*listptr = lappend(*listptr, node);
		return false;
	}
	return expression_tree_walker(node, pull_agg_clause_walker,
								  (void *) listptr);
}

/*
 * check_having_for_ungrouped_vars takes the havingQual and the list of
 * GROUP BY clauses and checks for subplans in the havingQual that are being
 * passed ungrouped variables as parameters.  In other contexts, ungrouped
 * vars in the havingQual will be detected by the parser (see parse_agg.c,
 * exprIsAggOrGroupCol()).	But that routine currently does not check subplans,
 * because the necessary info is not computed until the planner runs.
 * This ought to be cleaned up someday.
 *
 * NOTE: the havingClause has been cnf-ified, so AND subclauses have been
 * turned into a plain List.  Thus, this routine has to cope with List nodes
 * where the routine above does not...
 */

void
check_having_for_ungrouped_vars(Node *clause, List *groupClause,
								List *targetList)
{
	check_having_for_ungrouped_vars_context context;

	context.groupClause = groupClause;
	context.targetList = targetList;
	check_having_for_ungrouped_vars_walker(clause, &context);
}

static bool
check_having_for_ungrouped_vars_walker(Node *node,
					check_having_for_ungrouped_vars_context *context)
{
	if (node == NULL)
		return false;
	/*
	 * We can ignore Vars other than in subplan args lists,
	 * since the parser already checked 'em.
	 */
	if (is_subplan(node))
	{
		/*
		 * The args list of the subplan node represents attributes from
		 * outside passed into the sublink.
		 */
		List	*t;

		foreach(t, ((Expr *) node)->args)
		{
			Node	   *thisarg = lfirst(t);
			bool		contained_in_group_clause = false;
			List	   *gl;

			foreach(gl, context->groupClause)
			{
				Var	   *groupexpr = get_groupclause_expr(lfirst(gl),
														 context->targetList);

				if (var_equal((Var *) thisarg, groupexpr))
				{
					contained_in_group_clause = true;
					break;
				}
			}

			if (!contained_in_group_clause)
				elog(ERROR, "Sub-SELECT in HAVING clause must use only GROUPed attributes from outer SELECT");
		}
	}
	return expression_tree_walker(node,
								  check_having_for_ungrouped_vars_walker,
								  (void *) context);
}
