/*-------------------------------------------------------------------------
 *
 * setrefs.c
 *	  Routines to change varno/attno entries to contain references
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/setrefs.c,v 1.46 1999/05/12 15:01:39 wieck Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "nodes/primnodes.h"
#include "nodes/relation.h"

#include "utils/elog.h"
#include "nodes/nodeFuncs.h"
#include "nodes/makefuncs.h"

#include "optimizer/internal.h"
#include "optimizer/clauses.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/keys.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "optimizer/tlist.h"

static void set_join_tlist_references(Join *join);
static void set_nonamescan_tlist_references(SeqScan *nonamescan);
static void set_noname_tlist_references(Noname *noname);
static Node *replace_clause_joinvar_refs(Node *clause,
										 List *outer_tlist,
										 List *inner_tlist);
static Var *replace_joinvar_refs(Var *var,
								 List *outer_tlist,
								 List *inner_tlist);
static List *tlist_noname_references(Oid nonameid, List *tlist);
static bool OperandIsInner(Node *opnd, int inner_relid);
static List *pull_agg_clause(Node *clause);
static Node *del_agg_clause(Node *clause);
static void set_result_tlist_references(Result *resultNode);

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
	else if (IsA(plan, SeqScan) &&plan->lefttree &&
			 IsA_Noname(plan->lefttree))
		set_nonamescan_tlist_references((SeqScan *) plan);
	else if (IsA(plan, Sort))
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
	Plan	   *outer = ((Plan *) join)->lefttree;
	Plan	   *inner = ((Plan *) join)->righttree;
	List	   *outer_tlist = ((outer == NULL) ? NIL : outer->targetlist);
	List	   *inner_tlist = ((inner == NULL) ? NIL : inner->targetlist);
	List	   *new_join_targetlist = NIL;
	List	   *qptlist = ((Plan *) join)->targetlist;
	List	   *entry;

	foreach(entry, qptlist)
	{
		TargetEntry *xtl = (TargetEntry *) lfirst(entry);
		Node *joinvar = replace_clause_joinvar_refs(xtl->expr,
													outer_tlist,
													inner_tlist);
		new_join_targetlist = lappend(new_join_targetlist,
									  makeTargetEntry(xtl->resdom, joinvar));
	}

	((Plan *) join)->targetlist = new_join_targetlist;
	if (outer != NULL)
		set_tlist_references(outer);
	if (inner != NULL)
		set_tlist_references(inner);
}

/*
 * set_nonamescan_tlist_references
 *	  Modifies the target list of a node that scans a noname relation (i.e., a
 *	  sort or hash node) so that the varnos refer to the child noname.
 *
 * 'nonamescan' is a seqscan node
 *
 * Returns nothing of interest, but modifies internal fields of nodes.
 *
 */
static void
set_nonamescan_tlist_references(SeqScan *nonamescan)
{
	Noname	   *noname = (Noname *) ((Plan *) nonamescan)->lefttree;

	((Plan *) nonamescan)->targetlist = tlist_noname_references(noname->nonameid,
							  ((Plan *) nonamescan)->targetlist);
	set_noname_tlist_references(noname);
}

/*
 * set_noname_tlist_references
 *	  The noname's vars are made consistent with (actually, identical to) the
 *	  modified version of the target list of the node from which noname node
 *	  receives its tuples.
 *
 * 'noname' is a noname (e.g., sort, hash) plan node
 *
 * Returns nothing of interest, but modifies internal fields of nodes.
 *
 */
static void
set_noname_tlist_references(Noname *noname)
{
	Plan	   *source = ((Plan *) noname)->lefttree;

	if (source != NULL)
	{
		set_tlist_references(source);
		((Plan *) noname)->targetlist = copy_vars(((Plan *) noname)->targetlist,
					  (source)->targetlist);
	}
	else
		elog(ERROR, "calling set_noname_tlist_references with empty lefttree");
}

/*
 * join_references
 *	   Creates a new set of join clauses by changing the varno/varattno
 *	   values of variables in the clauses to reference target list values
 *	   from the outer and inner join relation target lists.
 *	   This is just an external interface for replace_clause_joinvar_refs.
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
	return (List *) replace_clause_joinvar_refs((Node *) clauses,
												outer_tlist,
												inner_tlist);
}

/*
 * index_outerjoin_references
 *	  Given a list of join clauses, replace the operand corresponding to the
 *	  outer relation in the join with references to the corresponding target
 *	  list element in 'outer_tlist' (the outer is rather obscurely
 *	  identified as the side that doesn't contain a var whose varno equals
 *	  'inner_relid').
 *
 *	  As a side effect, the operator is replaced by the regproc id.
 *
 * 'inner_indxqual' is the list of join clauses (so-called because they
 * are used as qualifications for the inner (inbex) scan of a nestloop)
 *
 * Returns the new list of clauses.
 *
 */
List *
index_outerjoin_references(List *inner_indxqual,
						   List *outer_tlist,
						   Index inner_relid)
{
	List	   *t_list = NIL;
	Expr	   *temp = NULL;
	List	   *t_clause = NIL;
	Expr	   *clause = NULL;

	foreach(t_clause, inner_indxqual)
	{
		clause = lfirst(t_clause);

		/*
		 * if inner scan on the right.
		 */
		if (OperandIsInner((Node *) get_rightop(clause), inner_relid))
		{
			Var		   *joinvar = (Var *)
				replace_clause_joinvar_refs((Node *) get_leftop(clause),
											outer_tlist,
											NIL);

			temp = make_opclause(replace_opid((Oper *) ((Expr *) clause)->oper),
								 joinvar,
								 get_rightop(clause));
			t_list = lappend(t_list, temp);
		}
		else
		{
			/* inner scan on left */
			Var		   *joinvar = (Var *)
				replace_clause_joinvar_refs((Node *) get_rightop(clause),
											outer_tlist,
											NIL);

			temp = make_opclause(replace_opid((Oper *) ((Expr *) clause)->oper),
								 get_leftop(clause),
								 joinvar);
			t_list = lappend(t_list, temp);
		}

	}
	return t_list;
}

/*
 * replace_clause_joinvar_refs
 * replace_joinvar_refs
 *
 *	  Replaces all variables within a join clause with a new var node
 *	  whose varno/varattno fields contain a reference to a target list
 *	  element from either the outer or inner join relation.
 *
 * 'clause' is the join clause
 * 'outer_tlist' is the target list of the outer join relation
 * 'inner_tlist' is the target list of the inner join relation
 *
 * Returns the new join clause.
 * NB: it is critical that the original clause structure not be modified!
 * The changes must be applied to a copy.
 *
 * XXX the current implementation does not copy unchanged primitive
 * nodes; they remain shared with the original.  Is this safe?
 */
static Node *
replace_clause_joinvar_refs(Node *clause,
							List *outer_tlist,
							List *inner_tlist)
{
	if (clause == NULL)
		return NULL;
	if (IsA(clause, Var))
	{
		Var	   *temp = replace_joinvar_refs((Var *) clause,
											outer_tlist, inner_tlist);
		if (temp != NULL)
			return (Node *) temp;
		else
			return clause;
	}
	else if (single_node(clause))
		return clause;
	else if (and_clause(clause))
	{
		return (Node *) make_andclause((List *)
			replace_clause_joinvar_refs((Node *) ((Expr *) clause)->args,
										outer_tlist,
										inner_tlist));
	}
	else if (or_clause(clause))
	{
		return (Node *) make_orclause((List *)
			replace_clause_joinvar_refs((Node *) ((Expr *) clause)->args,
										outer_tlist,
										inner_tlist));
	}
	else if (IsA(clause, ArrayRef))
	{
		ArrayRef   *oldnode = (ArrayRef *) clause;
		ArrayRef   *newnode = makeNode(ArrayRef);

		newnode->refattrlength = oldnode->refattrlength;
		newnode->refelemlength = oldnode->refelemlength;
		newnode->refelemtype = oldnode->refelemtype;
		newnode->refelembyval = oldnode->refelembyval;
		newnode->refupperindexpr = (List *)
			replace_clause_joinvar_refs((Node *) oldnode->refupperindexpr,
										outer_tlist,
										inner_tlist);
		newnode->reflowerindexpr = (List *)
			replace_clause_joinvar_refs((Node *) oldnode->reflowerindexpr,
										outer_tlist,
										inner_tlist);
		newnode->refexpr =
			replace_clause_joinvar_refs(oldnode->refexpr,
										outer_tlist,
										inner_tlist);
		newnode->refassgnexpr =
			replace_clause_joinvar_refs(oldnode->refassgnexpr,
										outer_tlist,
										inner_tlist);

		return (Node *) newnode;
	}
	else if (is_funcclause(clause))
	{
		return (Node *) make_funcclause(
			(Func *) ((Expr *) clause)->oper,
			(List *) replace_clause_joinvar_refs(
				(Node *) ((Expr *) clause)->args,
				outer_tlist,
				inner_tlist));
	}
	else if (not_clause(clause))
	{
		return (Node *) make_notclause((Expr *)
				replace_clause_joinvar_refs(
					(Node *) get_notclausearg((Expr *) clause),
					outer_tlist,
					inner_tlist));
	}
	else if (is_opclause(clause))
	{
		return (Node *) make_opclause(
			replace_opid((Oper *) ((Expr *) clause)->oper),
			(Var *) replace_clause_joinvar_refs(
				(Node *) get_leftop((Expr *) clause),
				outer_tlist,
				inner_tlist),
			(Var *) replace_clause_joinvar_refs(
				(Node *) get_rightop((Expr *) clause),
				outer_tlist,
				inner_tlist));
	}
	else if (IsA(clause, List))
	{
		List	   *t_list = NIL;
		List	   *subclause;

		foreach(subclause, (List *) clause)
		{
			t_list = lappend(t_list,
							 replace_clause_joinvar_refs(lfirst(subclause),
														 outer_tlist,
														 inner_tlist));
		}
		return (Node *) t_list;
	}
	else if (is_subplan(clause))
	{
		/* This is a tad wasteful of space, but it works... */
		Expr *newclause = (Expr *) copyObject(clause);
		newclause->args = (List *)
			replace_clause_joinvar_refs((Node *) newclause->args,
										outer_tlist,
										inner_tlist);
		((SubPlan *) newclause->oper)->sublink->oper = (List *)
			replace_clause_joinvar_refs(
				(Node *) ((SubPlan *) newclause->oper)->sublink->oper,
				outer_tlist,
				inner_tlist);
		return (Node *) newclause;
	}
	else if (IsA(clause, CaseExpr))
	{
		CaseExpr   *oldnode = (CaseExpr *) clause;
		CaseExpr   *newnode = makeNode(CaseExpr);

		newnode->casetype = oldnode->casetype;
		newnode->arg = oldnode->arg; /* XXX should always be null anyway ... */
		newnode->args = (List *)
			replace_clause_joinvar_refs((Node *) oldnode->args,
										outer_tlist,
										inner_tlist);
		newnode->defresult =
			replace_clause_joinvar_refs(oldnode->defresult,
										outer_tlist,
										inner_tlist);

		return (Node *) newnode;
	}
	else if (IsA(clause, CaseWhen))
	{
		CaseWhen   *oldnode = (CaseWhen *) clause;
		CaseWhen   *newnode = makeNode(CaseWhen);

		newnode->expr =
			replace_clause_joinvar_refs(oldnode->expr,
										outer_tlist,
										inner_tlist);
		newnode->result =
			replace_clause_joinvar_refs(oldnode->result,
										outer_tlist,
										inner_tlist);

		return (Node *) newnode;
	}
	else
	{
		elog(ERROR, "replace_clause_joinvar_refs: unsupported clause %d",
			 nodeTag(clause));
		return NULL;
	}
}

static Var *
replace_joinvar_refs(Var *var, List *outer_tlist, List *inner_tlist)
{
	Resdom	   *outer_resdom;

	outer_resdom = tlist_member(var, outer_tlist);

	if (outer_resdom != NULL && IsA(outer_resdom, Resdom))
	{
		return (makeVar(OUTER,
						outer_resdom->resno,
						var->vartype,
						var->vartypmod,
						0,
						var->varnoold,
						var->varoattno));
	}
	else
	{
		Resdom	   *inner_resdom;

		inner_resdom = tlist_member(var, inner_tlist);
		if (inner_resdom != NULL && IsA(inner_resdom, Resdom))
		{
			return (makeVar(INNER,
							inner_resdom->resno,
							var->vartype,
							var->vartypmod,
							0,
							var->varnoold,
							var->varoattno));
		}
	}
	return (Var *) NULL;
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
	TargetEntry *noname = (TargetEntry *) NULL;
	TargetEntry *xtl = NULL;
	List	   *entry;

	foreach(entry, tlist)
	{
		AttrNumber	oattno;

		xtl = lfirst(entry);
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
 *	   it is always nil
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
	List   *t;

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
void
replace_vars_with_subplan_refs(Node *clause,
							   Index subvarno,
							   List *subplanTargetList)
{
	List	   *t;

	if (clause == NULL)
		return;
	if (IsA(clause, Var))
	{
		/*
		 * Ha! A Var node!
		 *
		 * It could be that this varnode has been created by make_groupplan
		 * and is already set up to reference the subplan target list.
		 * We recognize that case by varno = 1, varnoold = -1,
		 * varattno = varoattno, and varlevelsup = 0.  (Probably ought to
		 * have an explicit flag, but this should do for now.)
		 */
		Var			*var = (Var *) clause;
		TargetEntry *subplanVar;

		if (var->varno == (Index) 1 && 
			var->varnoold == ((Index) -1) &&
			var->varattno == var->varoattno &&
			var->varlevelsup == 0)
			return;				/* OK to leave it alone */

		/* Otherwise it had better be in the subplan list. */
		subplanVar = match_varid(var, subplanTargetList);
		if (! subplanVar)
			elog(ERROR, "replace_vars_with_subplan_refs: variable not in target list");

		/*
		 * Change the varno & varattno fields of the var node.
		 */
		var->varno = subvarno;
		var->varattno = subplanVar->resdom->resno;
	}
	else if (single_node(clause))
	{
		/* do nothing! */
	}
	else if (IsA(clause, Iter))
		replace_vars_with_subplan_refs(((Iter *) clause)->iterexpr,
									   subvarno, subplanTargetList);
	else if (is_subplan(clause))
	{
		foreach(t, ((Expr *) clause)->args)
			replace_vars_with_subplan_refs(lfirst(t),
										   subvarno, subplanTargetList);
		foreach(t, ((SubPlan *) ((Expr *) clause)->oper)->sublink->oper)
			replace_vars_with_subplan_refs(lfirst(((Expr *) lfirst(t))->args),
										   subvarno, subplanTargetList);
	}
	else if (IsA(clause, Expr))
	{
		/*
		 * Recursively scan the arguments of an expression.
		 * NOTE: this must come after is_subplan() case since
		 * subplan is a kind of Expr node.
		 */
		foreach(t, ((Expr *) clause)->args)
			replace_vars_with_subplan_refs(lfirst(t),
										   subvarno, subplanTargetList);
	}
	else if (IsA(clause, Aggref))
		replace_vars_with_subplan_refs(((Aggref *) clause)->target,
									   subvarno, subplanTargetList);
	else if (IsA(clause, ArrayRef))
	{
		ArrayRef   *aref = (ArrayRef *) clause;
		foreach(t, aref->refupperindexpr)
			replace_vars_with_subplan_refs(lfirst(t),
										   subvarno, subplanTargetList);
		foreach(t, aref->reflowerindexpr)
			replace_vars_with_subplan_refs(lfirst(t),
										   subvarno, subplanTargetList);
		replace_vars_with_subplan_refs(aref->refexpr,
									   subvarno, subplanTargetList);
		replace_vars_with_subplan_refs(aref->refassgnexpr,
									   subvarno, subplanTargetList);
	}
	else if (case_clause(clause))
	{
		foreach(t, ((CaseExpr *) clause)->args)
		{
			CaseWhen   *when = (CaseWhen *) lfirst(t);
			replace_vars_with_subplan_refs(when->expr,
										   subvarno, subplanTargetList);
			replace_vars_with_subplan_refs(when->result,
										   subvarno, subplanTargetList);
		}
		replace_vars_with_subplan_refs(((CaseExpr *) clause)->defresult,
									   subvarno, subplanTargetList);
	}
	else
	{
		elog(ERROR, "replace_vars_with_subplan_refs: Cannot handle node type %d",
			 nodeTag(clause));
	}
}

static bool
OperandIsInner(Node *opnd, int inner_relid)
{

	/*
	 * Can be the inner scan if its a varnode or a function and the
	 * inner_relid is equal to the varnode's var number or in the case of
	 * a function the first argument's var number (all args in a
	 * functional index are from the same relation).
	 */
	if (IsA(opnd, Var) &&
		(inner_relid == ((Var *) opnd)->varno))
		return true;
	if (is_funcclause(opnd))
	{
		List	   *firstArg = lfirst(((Expr *) opnd)->args);

		if (IsA(firstArg, Var) &&
			(inner_relid == ((Var *) firstArg)->varno))
			return true;
	}
	return false;
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
		Node *qual = lfirst(ql);
		List *qualaggs;

		replace_vars_with_subplan_refs(qual,
									   (Index) 0,
									   subplanTargetList);
		qualaggs = pull_agg_clause(qual);
		if (qualaggs == NIL)
			all_quals_ok = false; /* this qual clause has no agg functions! */
		else
			aggNode->aggs = nconc(qualaggs, aggNode->aggs);
	}

	return all_quals_ok;
}

/*
 * Make a list of all Aggref nodes contained in the given expression.
 */
static List *
pull_agg_clause(Node *clause)
{
	List	   *agg_list = NIL;
	List	   *t;

	if (clause == NULL)
		return NIL;
	else if (single_node(clause))
		return NIL;
	else if (IsA(clause, Iter))
		return pull_agg_clause(((Iter *) clause)->iterexpr);
	else if (is_subplan(clause))
	{
		SubLink *sublink = ((SubPlan *) ((Expr *) clause)->oper)->sublink;

		/*
		 * Only the lefthand side of the sublink should be checked for
		 * aggregates to be attached to the aggs list
		 */
		foreach(t, sublink->lefthand)
			agg_list = nconc(pull_agg_clause(lfirst(t)), agg_list);
		/* The first argument of ...->oper has also to be checked */
		foreach(t, sublink->oper)
			agg_list = nconc(pull_agg_clause(lfirst(t)), agg_list);
	}
	else if (IsA(clause, Expr))
	{
		/*
		 * Recursively scan the arguments of an expression.
		 * NOTE: this must come after is_subplan() case since
		 * subplan is a kind of Expr node.
		 */
		foreach(t, ((Expr *) clause)->args)
			agg_list = nconc(pull_agg_clause(lfirst(t)), agg_list);
	}
	else if (IsA(clause, Aggref))
	{
		return lcons(clause,
					 pull_agg_clause(((Aggref *) clause)->target));
	}
	else if (IsA(clause, ArrayRef))
	{
		ArrayRef   *aref = (ArrayRef *) clause;
		foreach(t, aref->refupperindexpr)
			agg_list = nconc(pull_agg_clause(lfirst(t)), agg_list);
		foreach(t, aref->reflowerindexpr)
			agg_list = nconc(pull_agg_clause(lfirst(t)), agg_list);
		agg_list = nconc(pull_agg_clause(aref->refexpr), agg_list);
		agg_list = nconc(pull_agg_clause(aref->refassgnexpr), agg_list);
	}
	else if (case_clause(clause))
	{
		foreach(t, ((CaseExpr *) clause)->args)
		{
			CaseWhen   *when = (CaseWhen *) lfirst(t);
			agg_list = nconc(agg_list, pull_agg_clause(when->expr));
			agg_list = nconc(agg_list, pull_agg_clause(when->result));
		}
		agg_list = nconc(pull_agg_clause(((CaseExpr *) clause)->defresult),
						 agg_list);
	}
	else
	{
		elog(ERROR, "pull_agg_clause: Cannot handle node type %d",
			 nodeTag(clause));
	}

	return agg_list;
}


/*
 * del_agg_tlist_references
 *	  Remove the Agg nodes from the target list
 *	  We do this so inheritance only does aggregates in the upper node
 */
void
del_agg_tlist_references(List *tlist)
{
	List	   *tl;

	foreach(tl, tlist)
	{
		TargetEntry *tle = lfirst(tl);

		tle->expr = del_agg_clause(tle->expr);
	}
}

static Node *
del_agg_clause(Node *clause)
{
	List	   *t;

	if (clause == NULL)
		return clause;

	if (IsA(clause, Var))
		return clause;
	else if (is_funcclause(clause))
	{

		/*
		 * This is a function. Recursively call this routine for its
		 * arguments...
		 */
		foreach(t, ((Expr *) clause)->args)
			lfirst(t) = del_agg_clause(lfirst(t));
	}
	else if (IsA(clause, Aggref))
	{

		/* here is the real action, to remove the Agg node */
		return del_agg_clause(((Aggref *) clause)->target);

	}
	else if (IsA(clause, ArrayRef))
	{
		ArrayRef   *aref = (ArrayRef *) clause;

		/*
		 * This is an arrayref. Recursively call this routine for its
		 * expression and its index expression...
		 */
		foreach(t, aref->refupperindexpr)
			lfirst(t) = del_agg_clause(lfirst(t));
		foreach(t, aref->reflowerindexpr)
			lfirst(t) = del_agg_clause(lfirst(t));
		aref->refexpr = del_agg_clause(aref->refexpr);
		aref->refassgnexpr = del_agg_clause(aref->refassgnexpr);
	}
	else if (is_opclause(clause))
	{

		/*
		 * This is an operator. Recursively call this routine for both its
		 * left and right operands
		 */
		Node	   *left = (Node *) get_leftop((Expr *) clause);
		Node	   *right = (Node *) get_rightop((Expr *) clause);

		if (left != (Node *) NULL)
			left = del_agg_clause(left);
		if (right != (Node *) NULL)
			right = del_agg_clause(right);
	}
	else if (IsA(clause, Param) ||IsA(clause, Const))
		return clause;
	else
	{

		/*
		 * Ooops! we can not handle that!
		 */
		elog(ERROR, "del_agg_clause: Can not handle this tlist!\n");
	}
	return NULL;
}

/*
 * check_having_for_ungrouped_vars takes the havingQual and the list of
 * GROUP BY clauses and checks for subplans in the havingQual that are being
 * passed ungrouped variables as parameters.  In other contexts, ungrouped
 * vars in the havingQual will be detected by the parser (see parse_agg.c,
 * exprIsAggOrGroupCol()).  But that routine currently does not check subplans,
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
	List	   *t;

	if (clause == NULL)
		return;

	if (IsA(clause, Var))
	{
		/* Ignore vars elsewhere in the having clause, since the
		 * parser already checked 'em.
		 */
	}
	else if (single_node(clause))
	{
		/* ignore */
	}
	else if (IsA(clause, Iter))
	{
		check_having_for_ungrouped_vars(((Iter *) clause)->iterexpr,
										groupClause, targetList);
	}
	else if (is_subplan(clause))
	{
		/*
		 * The args list of the subplan node represents attributes from outside
		 * passed into the sublink.
		 */
		foreach(t, ((Expr *) clause)->args)
		{
			bool contained_in_group_clause = false;
			List	   *gl;

			foreach(gl, groupClause)
			{
				if (var_equal(lfirst(t),
							  get_groupclause_expr((GroupClause *) 
							  					lfirst(gl), targetList)))
				{
					contained_in_group_clause = true;
					break;
				}
			}

			if (!contained_in_group_clause)
				elog(ERROR, "Sub-SELECT in HAVING clause must use only GROUPed attributes from outer SELECT");
		}
	}
	else if (IsA(clause, Expr))
	{
		/*
		 * Recursively scan the arguments of an expression.
		 * NOTE: this must come after is_subplan() case since
		 * subplan is a kind of Expr node.
		 */
		foreach(t, ((Expr *) clause)->args)
			check_having_for_ungrouped_vars(lfirst(t), groupClause,
														targetList);
	}
	else if (IsA(clause, List))
	{
		/*
		 * Recursively scan AND subclauses (see NOTE above).
		 */
		foreach(t, ((List *) clause))
			check_having_for_ungrouped_vars(lfirst(t), groupClause,
														targetList);
	}
	else if (IsA(clause, Aggref))
	{
		check_having_for_ungrouped_vars(((Aggref *) clause)->target,
										groupClause, targetList);
	}
	else if (IsA(clause, ArrayRef))
	{
		ArrayRef   *aref = (ArrayRef *) clause;

		/*
		 * This is an arrayref. Recursively call this routine for its
		 * expression and its index expression...
		 */
		foreach(t, aref->refupperindexpr)
			check_having_for_ungrouped_vars(lfirst(t), groupClause,
														targetList);
		foreach(t, aref->reflowerindexpr)
			check_having_for_ungrouped_vars(lfirst(t), groupClause,
														targetList);
		check_having_for_ungrouped_vars(aref->refexpr, groupClause,
														targetList);
		check_having_for_ungrouped_vars(aref->refassgnexpr, groupClause,
														targetList);
	}
	else if (case_clause(clause))
	{
		foreach(t, ((CaseExpr *) clause)->args)
		{
			CaseWhen   *when = (CaseWhen *) lfirst(t);
			check_having_for_ungrouped_vars(when->expr, groupClause,
														targetList);
			check_having_for_ungrouped_vars(when->result, groupClause,
														targetList);
		}
		check_having_for_ungrouped_vars(((CaseExpr *) clause)->defresult,
										groupClause, targetList);
	}
	else
	{
		elog(ERROR, "check_having_for_ungrouped_vars: Cannot handle node type %d",
			 nodeTag(clause));
	}
}
