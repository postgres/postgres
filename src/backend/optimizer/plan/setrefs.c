/*-------------------------------------------------------------------------
 *
 * setrefs.c--
 *	  Routines to change varno/attno entries to contain references
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/setrefs.c,v 1.31 1999/01/23 23:28:08 momjian Exp $
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
#include "optimizer/clauseinfo.h"
#include "optimizer/keys.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "optimizer/tlist.h"

static void set_join_tlist_references(Join *join);
static void set_tempscan_tlist_references(SeqScan *tempscan);
static void set_temp_tlist_references(Temp *temp);
static List *replace_clause_joinvar_refs(Expr *clause,
							List *outer_tlist, List *inner_tlist);
static List *replace_subclause_joinvar_refs(List *clauses,
							   List *outer_tlist, List *inner_tlist);
static Var *replace_joinvar_refs(Var *var, List *outer_tlist, List *inner_tlist);
static List *tlist_temp_references(Oid tempid, List *tlist);
static void replace_result_clause(Node *clause, List *subplanTargetList);
static bool OperandIsInner(Node *opnd, int inner_relid);
static List *replace_agg_clause(Node *expr, List *targetlist);
static Node *del_agg_clause(Node *clause);
static void set_result_tlist_references(Result *resultNode);

/*****************************************************************************
 *
 *		SUBPLAN REFERENCES
 *
 *****************************************************************************/

/*
 * set-tlist-references--
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
			 IsA_Temp(plan->lefttree))
		set_tempscan_tlist_references((SeqScan *) plan);
	else if (IsA(plan, Sort))
		set_temp_tlist_references((Temp *) plan);
	else if (IsA(plan, Result))
		set_result_tlist_references((Result *) plan);
	else if (IsA(plan, Hash))
		set_tlist_references(plan->lefttree);
}

/*
 * set-join-tlist-references--
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
	List	   *new_join_targetlist = NIL;
	TargetEntry *temp = (TargetEntry *) NULL;
	List	   *entry = NIL;
	List	   *inner_tlist = NULL;
	List	   *outer_tlist = NULL;
	TargetEntry *xtl = (TargetEntry *) NULL;
	List	   *qptlist = ((Plan *) join)->targetlist;

	foreach(entry, qptlist)
	{
		List	   *joinvar;

		xtl = (TargetEntry *) lfirst(entry);
		inner_tlist = ((inner == NULL) ? NIL : inner->targetlist);
		outer_tlist = ((outer == NULL) ? NIL : outer->targetlist);
		joinvar = replace_clause_joinvar_refs((Expr *) get_expr(xtl),
											  outer_tlist,
											  inner_tlist);

		temp = makeTargetEntry(xtl->resdom, (Node *) joinvar);
		new_join_targetlist = lappend(new_join_targetlist, temp);
	}

	((Plan *) join)->targetlist = new_join_targetlist;
	if (outer != NULL)
		set_tlist_references(outer);
	if (inner != NULL)
		set_tlist_references(inner);
}

/*
 * set-tempscan-tlist-references--
 *	  Modifies the target list of a node that scans a temp relation (i.e., a
 *	  sort or hash node) so that the varnos refer to the child temporary.
 *
 * 'tempscan' is a seqscan node
 *
 * Returns nothing of interest, but modifies internal fields of nodes.
 *
 */
static void
set_tempscan_tlist_references(SeqScan *tempscan)
{
	Temp	   *temp = (Temp *) ((Plan *) tempscan)->lefttree;

	((Plan *) tempscan)->targetlist =
		tlist_temp_references(temp->tempid,
							  ((Plan *) tempscan)->targetlist);
	set_temp_tlist_references(temp);
}

/*
 * set-temp-tlist-references--
 *	  The temp's vars are made consistent with (actually, identical to) the
 *	  modified version of the target list of the node from which temp node
 *	  receives its tuples.
 *
 * 'temp' is a temp (e.g., sort, hash) plan node
 *
 * Returns nothing of interest, but modifies internal fields of nodes.
 *
 */
static void
set_temp_tlist_references(Temp *temp)
{
	Plan	   *source = ((Plan *) temp)->lefttree;

	if (source != NULL)
	{
		set_tlist_references(source);
		((Plan *) temp)->targetlist =
			copy_vars(((Plan *) temp)->targetlist,
					  (source)->targetlist);
	}
	else
		elog(ERROR, "calling set_temp_tlist_references with empty lefttree");
}

/*
 * join-references--
 *	   Creates a new set of join clauses by replacing the varno/varattno
 *	   values of variables in the clauses to reference target list values
 *	   from the outer and inner join relation target lists.
 *
 * 'clauses' is the list of join clauses
 * 'outer-tlist' is the target list of the outer join relation
 * 'inner-tlist' is the target list of the inner join relation
 *
 * Returns the new join clauses.
 *
 */
List *
join_references(List *clauses,
				List *outer_tlist,
				List *inner_tlist)
{
	return (replace_subclause_joinvar_refs(clauses,
										   outer_tlist,
										   inner_tlist));
}

/*
 * index-outerjoin-references--
 *	  Given a list of join clauses, replace the operand corresponding to the
 *	  outer relation in the join with references to the corresponding target
 *	  list element in 'outer-tlist' (the outer is rather obscurely
 *	  identified as the side that doesn't contain a var whose varno equals
 *	  'inner-relid').
 *
 *	  As a side effect, the operator is replaced by the regproc id.
 *
 * 'inner-indxqual' is the list of join clauses (so-called because they
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
			replace_clause_joinvar_refs((Expr *) get_leftop(clause),
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
			replace_clause_joinvar_refs((Expr *) get_rightop(clause),
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
 * replace-clause-joinvar-refs
 * replace-subclause-joinvar-refs
 * replace-joinvar-refs
 *
 *	  Replaces all variables within a join clause with a new var node
 *	  whose varno/varattno fields contain a reference to a target list
 *	  element from either the outer or inner join relation.
 *
 * 'clause' is the join clause
 * 'outer-tlist' is the target list of the outer join relation
 * 'inner-tlist' is the target list of the inner join relation
 *
 * Returns the new join clause.
 *
 */
static List *
replace_clause_joinvar_refs(Expr *clause,
							List *outer_tlist,
							List *inner_tlist)
{
	List	   *temp = NULL;

	if (IsA(clause, Var))
	{
		temp = (List *) replace_joinvar_refs((Var *) clause,
											 outer_tlist, inner_tlist);
		if (temp != NULL)
			return temp;
		else if (clause != NULL)
			return (List *) clause;
		else
			return NIL;
	}
	else if (single_node((Node *) clause))
		return (List *) clause;
	else if (and_clause((Node *) clause))
	{
		List	   *andclause =
		replace_subclause_joinvar_refs(((Expr *) clause)->args,
									   outer_tlist,
									   inner_tlist);

		return (List *) make_andclause(andclause);
	}
	else if (or_clause((Node *) clause))
	{
		List	   *orclause =
		replace_subclause_joinvar_refs(((Expr *) clause)->args,
									   outer_tlist,
									   inner_tlist);

		return (List *) make_orclause(orclause);
	}
	else if (IsA(clause, ArrayRef))
	{
		ArrayRef   *aref = (ArrayRef *) clause;

		temp = replace_subclause_joinvar_refs(aref->refupperindexpr,
											  outer_tlist,
											  inner_tlist);
		aref->refupperindexpr = (List *) temp;
		temp = replace_subclause_joinvar_refs(aref->reflowerindexpr,
											  outer_tlist,
											  inner_tlist);
		aref->reflowerindexpr = (List *) temp;
		temp = replace_clause_joinvar_refs((Expr *) aref->refexpr,
										   outer_tlist,
										   inner_tlist);
		aref->refexpr = (Node *) temp;

		/*
		 * no need to set refassgnexpr.  we only set that in the target
		 * list on replaces, and this is an array reference in the
		 * qualification.  if we got this far, it's 0x0 in the ArrayRef
		 * structure 'clause'.
		 */

		return (List *) clause;
	}
	else if (is_funcclause((Node *) clause))
	{
		List	   *funcclause =
		replace_subclause_joinvar_refs(((Expr *) clause)->args,
									   outer_tlist,
									   inner_tlist);

		return ((List *) make_funcclause((Func *) ((Expr *) clause)->oper,
										 funcclause));
	}
	else if (not_clause((Node *) clause))
	{
		List	   *notclause =
		replace_clause_joinvar_refs(get_notclausearg(clause),
									outer_tlist,
									inner_tlist);

		return (List *) make_notclause((Expr *) notclause);
	}
	else if (is_opclause((Node *) clause))
	{
		Var		   *leftvar =
		(Var *) replace_clause_joinvar_refs((Expr *) get_leftop(clause),
											outer_tlist,
											inner_tlist);
		Var		   *rightvar =
		(Var *) replace_clause_joinvar_refs((Expr *) get_rightop(clause),
											outer_tlist,
											inner_tlist);

		return ((List *) make_opclause(replace_opid((Oper *) ((Expr *) clause)->oper),
									   leftvar,
									   rightvar));
	}
	else if (is_subplan(clause))
	{
		((Expr *) clause)->args =
			replace_subclause_joinvar_refs(((Expr *) clause)->args,
										   outer_tlist,
										   inner_tlist);
		((SubPlan *) ((Expr *) clause)->oper)->sublink->oper =
			replace_subclause_joinvar_refs(((SubPlan *) ((Expr *) clause)->oper)->sublink->oper,
										   outer_tlist,
										   inner_tlist);
		return (List *) clause;
	}
	else if (IsA(clause, CaseExpr))
	{
		((CaseExpr *) clause)->args =
			(List *) replace_subclause_joinvar_refs(((CaseExpr *) clause)->args,
													outer_tlist,
													inner_tlist);

		((CaseExpr *) clause)->defresult =
			(Node *) replace_clause_joinvar_refs((Expr *) ((CaseExpr *) clause)->defresult,
												 outer_tlist,
												 inner_tlist);
		return (List *) clause;
	}
	else if (IsA(clause, CaseWhen))
	{
		((CaseWhen *) clause)->expr =
			(Node *) replace_clause_joinvar_refs((Expr *) ((CaseWhen *) clause)->expr,
												 outer_tlist,
												 inner_tlist);

		((CaseWhen *) clause)->result =
			(Node *) replace_clause_joinvar_refs((Expr *) ((CaseWhen *) clause)->result,
												 outer_tlist,
												 inner_tlist);
		return (List *) clause;
	}

	/* shouldn't reach here */
	elog(ERROR, "replace_clause_joinvar_refs: unsupported clause %d",
		 nodeTag(clause));
	return NULL;
}

static List *
replace_subclause_joinvar_refs(List *clauses,
							   List *outer_tlist,
							   List *inner_tlist)
{
	List	   *t_list = NIL;
	List	   *temp = NIL;
	List	   *clause = NIL;

	foreach(clause, clauses)
	{
		temp = replace_clause_joinvar_refs(lfirst(clause),
										   outer_tlist,
										   inner_tlist);
		t_list = lappend(t_list, temp);
	}
	return t_list;
}

static Var *
replace_joinvar_refs(Var *var, List *outer_tlist, List *inner_tlist)
{
	Resdom	   *outer_resdom = (Resdom *) NULL;

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
 * tlist-temp-references--
 *	  Creates a new target list for a node that scans a temp relation,
 *	  setting the varnos to the id of the temp relation and setting varids
 *	  if necessary (varids are only needed if this is a targetlist internal
 *	  to the tree, in which case the targetlist entry always contains a var
 *	  node, so we can just copy it from the temp).
 *
 * 'tempid' is the id of the temp relation
 * 'tlist' is the target list to be modified
 *
 * Returns new target list
 *
 */
static List *
tlist_temp_references(Oid tempid,
					  List *tlist)
{
	List	   *t_list = NIL;
	TargetEntry *temp = (TargetEntry *) NULL;
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

		temp = makeTargetEntry(xtl->resdom,
							   (Node *) makeVar(tempid,
												xtl->resdom->resno,
												xtl->resdom->restype,
												xtl->resdom->restypmod,
												0,
												tempid,
												oattno));

		t_list = lappend(t_list, temp);
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
	List	   *t;
	TargetEntry *entry;
	Expr	   *expr;

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

	/*
	 * now for traverse all the entris of the target list. These should be
	 * of the form (Resdom_Node Expression). For every expression clause,
	 * call "replace_result_clause()" to appropriatelly change all the Var
	 * nodes.
	 */
	foreach(t, resultTargetList)
	{
		entry = (TargetEntry *) lfirst(t);
		expr = (Expr *) get_expr(entry);
		replace_result_clause((Node *) expr, subplanTargetList);
	}
}

/*---------------------------------------------------------
 *
 * replace_result_clause
 *
 * This routine is called from set_result_tlist_references().
 * and modifies the expressions of the target list of a Result
 * node so that all Var nodes reference the target list of its subplan.
 *
 */
static void
replace_result_clause(Node *clause,
					  List *subplanTargetList)	/* target list of the
												 * subplan */
{
	List	   *t;

	if (IsA(clause, Var))
	{
		TargetEntry *subplanVar;

		/*
		 * Ha! A Var node!
		 */
		subplanVar = match_varid((Var *) clause, subplanTargetList);

		/*
		 * Change the varno & varattno fields of the var node.
		 *
		 */
		((Var *) clause)->varno = (Index) OUTER;
		((Var *) clause)->varattno = subplanVar->resdom->resno;
	}
	else if (IsA(clause, Aggreg))
		replace_result_clause(((Aggreg *) clause)->target, subplanTargetList);
	else if (is_funcclause(clause))
	{
		List	   *subExpr;

		/*
		 * This is a function. Recursively call this routine for its
		 * arguments...
		 */
		subExpr = ((Expr *) clause)->args;
		foreach(t, subExpr)
			replace_result_clause(lfirst(t), subplanTargetList);
	}
	else if (IsA(clause, ArrayRef))
	{
		ArrayRef   *aref = (ArrayRef *) clause;

		/*
		 * This is an arrayref. Recursively call this routine for its
		 * expression and its index expression...
		 */
		foreach(t, aref->refupperindexpr)
			replace_result_clause(lfirst(t), subplanTargetList);
		foreach(t, aref->reflowerindexpr)
			replace_result_clause(lfirst(t), subplanTargetList);
		replace_result_clause(aref->refexpr,
							  subplanTargetList);
		replace_result_clause(aref->refassgnexpr,
							  subplanTargetList);
	}
	else if (is_opclause(clause))
	{
		Node	   *subNode;

		/*
		 * This is an operator. Recursively call this routine for both its
		 * left and right operands
		 */
		subNode = (Node *) get_leftop((Expr *) clause);
		replace_result_clause(subNode, subplanTargetList);
		subNode = (Node *) get_rightop((Expr *) clause);
		replace_result_clause(subNode, subplanTargetList);
	}
	else if (IsA(clause, Param) ||IsA(clause, Const))
	{
		/* do nothing! */
	}
	else
	{

		/*
		 * Ooops! we can not handle that!
		 */
		elog(ERROR, "replace_result_clause: Can not handle this tlist!\n");
	}
}

static
bool
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
 *	  changes the target list of an Agg node so that it points to
 *	  the tuples returned by its left tree subplan.
 *
 *	We now also generate a linked list of Aggreg pointers for Agg.
 *
 */
List *
set_agg_tlist_references(Agg *aggNode)
{
	List	   *aggTargetList;
	List	   *subplanTargetList;
	List	   *tl;
	List	   *aggreg_list = NIL;

	aggTargetList = aggNode->plan.targetlist;
	subplanTargetList = aggNode->plan.lefttree->targetlist;

	foreach(tl, aggTargetList)
	{
		TargetEntry *tle = lfirst(tl);

		aggreg_list = nconc(
		  replace_agg_clause(tle->expr, subplanTargetList), aggreg_list);
	}
	return aggreg_list;
}

static List *
replace_agg_clause(Node *clause, List *subplanTargetList)
{
	List	   *t;
	List	   *agg_list = NIL;

	if (IsA(clause, Var))
	{
		TargetEntry *subplanVar;

		/*
		 * Ha! A Var node!
		 */
		subplanVar = match_varid((Var *) clause, subplanTargetList);

		/*
		 * Change the varno & varattno fields of the var node.
		 *
		 */
		((Var *) clause)->varattno = subplanVar->resdom->resno;

		return NIL;
	}
	else if (is_funcclause(clause))
	{

		/*
		 * This is a function. Recursively call this routine for its
		 * arguments...
		 */
		foreach(t, ((Expr *) clause)->args)
		{
			agg_list = nconc(agg_list,
					   replace_agg_clause(lfirst(t), subplanTargetList));
		}
		return agg_list;
	}
	else if (IsA(clause, Aggreg))
	{
		return lcons(clause,
					 replace_agg_clause(((Aggreg *) clause)->target, subplanTargetList));
	}
	else if (IsA(clause, ArrayRef))
	{
		ArrayRef   *aref = (ArrayRef *) clause;

		/*
		 * This is an arrayref. Recursively call this routine for its
		 * expression and its index expression...
		 */
		foreach(t, aref->refupperindexpr)
		{
			agg_list = nconc(agg_list,
					   replace_agg_clause(lfirst(t), subplanTargetList));
		}
		foreach(t, aref->reflowerindexpr)
		{
			agg_list = nconc(agg_list,
					   replace_agg_clause(lfirst(t), subplanTargetList));
		}
		agg_list = nconc(agg_list,
				   replace_agg_clause(aref->refexpr, subplanTargetList));
		agg_list = nconc(agg_list,
			  replace_agg_clause(aref->refassgnexpr, subplanTargetList));

		return agg_list;
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
			agg_list = nconc(agg_list,
							 replace_agg_clause(left, subplanTargetList));
		if (right != (Node *) NULL)
			agg_list = nconc(agg_list,
						   replace_agg_clause(right, subplanTargetList));

		return agg_list;
	}
	else if (IsA(clause, Param) ||IsA(clause, Const))
	{
		/* do nothing! */
		return NIL;
	}
	else
	{

		/*
		 * Ooops! we can not handle that!
		 */
		elog(ERROR, "replace_agg_clause: Can not handle this tlist!\n");
		return NIL;
	}
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
	else if (IsA(clause, Aggreg))
	{

		/* here is the real action, to remove the Agg node */
		return del_agg_clause(((Aggreg *) clause)->target);

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

/***S*H***/ 
/* check_having_qual_for_vars takes the the havingQual and the actual targetlist as arguments
 * and recursively scans the havingQual for attributes that are not included in the targetlist
 * yet. Attributes contained in the havingQual but not in the targetlist show up with queries
 * like: 
 * SELECT sid 
 * FROM part
 * GROUP BY sid
 * HAVING MIN(pid) > 1;  (pid is used but never selected for!!!). 
 * To be able to handle queries like that correctly we have to extend the actual targetlist
 *  (which will be the one used for the GROUP node later on) by these attributes. */
List *
check_having_qual_for_vars(Node *clause, List *targetlist_so_far)
{
  List	   *t;


  if (IsA(clause, Var))
    {
      RelOptInfo         tmp_rel;
      

      tmp_rel.targetlist = targetlist_so_far;
      
      /* 
       * Ha! A Var node!
       */

      /* Check if the VAR is already contained in the targetlist */
      if (tlist_member((Var *)clause, (List *)targetlist_so_far) == NULL)
	{
	  add_tl_element(&tmp_rel, (Var *)clause); 
	} 
	    
      return tmp_rel.targetlist;
    }
  
  else if (is_funcclause(clause) || not_clause(clause) || 
	   or_clause(clause) || and_clause(clause))
    {
      
      /*
       * This is a function. Recursively call this routine for its
       * arguments...
       */
      foreach(t, ((Expr *) clause)->args)
	{
	  targetlist_so_far = check_having_qual_for_vars(lfirst(t), targetlist_so_far);
	}
      return targetlist_so_far;
    }
  else if (IsA(clause, Aggreg))
    {
	  targetlist_so_far = 
	    check_having_qual_for_vars(((Aggreg *) clause)->target, targetlist_so_far);
	  return targetlist_so_far;
    }
  else if (IsA(clause, ArrayRef))
    {
      ArrayRef   *aref = (ArrayRef *) clause;
      
      /*
       * This is an arrayref. Recursively call this routine for its
       * expression and its index expression...
       */
      foreach(t, aref->refupperindexpr)
	{
	  targetlist_so_far = check_having_qual_for_vars(lfirst(t), targetlist_so_far);
	}
      foreach(t, aref->reflowerindexpr)
	{
	  targetlist_so_far = check_having_qual_for_vars(lfirst(t), targetlist_so_far);
	}
      targetlist_so_far = check_having_qual_for_vars(aref->refexpr, targetlist_so_far);
      targetlist_so_far = check_having_qual_for_vars(aref->refassgnexpr, targetlist_so_far);
      
      return targetlist_so_far;
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
	targetlist_so_far = check_having_qual_for_vars(left, targetlist_so_far);
      if (right != (Node *) NULL)
	targetlist_so_far = check_having_qual_for_vars(right, targetlist_so_far);
      
      return targetlist_so_far;
    }
  else if (IsA(clause, Param) || IsA(clause, Const))
    {
      /* do nothing! */
      return targetlist_so_far;
    }
  /* If we get to a sublink, then we only have to check the lefthand side of the expression
   * to see if there are any additional VARs */
  else if (IsA(clause, SubLink))
    {
      foreach(t,((List *)((SubLink *)clause)->lefthand))
	{
	  targetlist_so_far = check_having_qual_for_vars(lfirst(t), targetlist_so_far);
	}
      return targetlist_so_far;
    }
  else
    {
      /*
       * Ooops! we can not handle that!
       */
      elog(ERROR, "check_having_qual_for_vars: Can not handle this having_qual! %d\n",
	   nodeTag(clause));
      return NIL;
    }
}

/* check_having_qual_for_aggs takes the havingQual, the targetlist and the groupClause  
 * as arguments and scans the havingQual recursively for aggregates. If an aggregate is
 * found it is attached to a list and returned by the function. (All the returned lists 
 * are concenated to result_plan->aggs in planner.c:union_planner() */
List *
check_having_qual_for_aggs(Node *clause, List *subplanTargetList, List *groupClause)
{
	List	   *t, *l1;
	List	   *agg_list = NIL;

	int contained_in_group_clause = 0;
	

	if (IsA(clause, Var))
	{
	  TargetEntry *subplanVar;
	  
	  /*
	   * Ha! A Var node!
	   */
	  subplanVar = match_varid((Var *) clause, subplanTargetList);
	  
	  /*
	   * Change the varno & varattno fields of the var node to point to the resdom->resno
	   * fields of the subplan (lefttree) 
	   */	  
	  ((Var *) clause)->varattno = subplanVar->resdom->resno;

	  return NIL;

	}
        /***S*H***/
	else if (is_funcclause(clause) || not_clause(clause) || 
		 or_clause(clause) || and_clause(clause))
	{
	  int new_length=0, old_length=0;
	  
		/*
		 * This is a function. Recursively call this routine for its
		 * arguments... (i.e. for AND, OR, ... clauses!)
		 */
		foreach(t, ((Expr *) clause)->args)
		{
		  old_length=length((List *)agg_list);

		  agg_list = nconc(agg_list,
				   check_having_qual_for_aggs(lfirst(t), subplanTargetList,
							      groupClause));

		  /* The arguments of OR or AND clauses are comparisons or relations 
		   * and because we are in the havingQual there must be at least one operand
		   * using an aggregate function. If so, we will find it and the length of the
		   * agg_list will be increased after the above call to 
		   * check_having_qual_for_aggs. If there are no aggregates used, the query
                   * could have been formulated using the 'where' clause */
		  if(((new_length=length((List *)agg_list)) == old_length) || (new_length == 0))
		    {
		      elog(ERROR,"This could have been done in a where clause!!");
		      return NIL;
		    } 
		}
		return agg_list;
	}
	else if (IsA(clause, Aggreg))
	{
		return lcons(clause,
		    check_having_qual_for_aggs(((Aggreg *) clause)->target, subplanTargetList,
					       groupClause));		
	}
	else if (IsA(clause, ArrayRef))
	{
		ArrayRef   *aref = (ArrayRef *) clause;

		/*
		 * This is an arrayref. Recursively call this routine for its
		 * expression and its index expression...
		 */
		foreach(t, aref->refupperindexpr)
		{
			agg_list = nconc(agg_list,
					 check_having_qual_for_aggs(lfirst(t), subplanTargetList,
								    groupClause));
		}
		foreach(t, aref->reflowerindexpr)
		{
			agg_list = nconc(agg_list,
					 check_having_qual_for_aggs(lfirst(t), subplanTargetList,
								    groupClause));
		}
		agg_list = nconc(agg_list,
				 check_having_qual_for_aggs(aref->refexpr, subplanTargetList,
							    groupClause));
		agg_list = nconc(agg_list,
				 check_having_qual_for_aggs(aref->refassgnexpr, subplanTargetList,
							    groupClause));

		return agg_list;
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
			agg_list = nconc(agg_list,
					 check_having_qual_for_aggs(left, subplanTargetList,
								    groupClause));
		if (right != (Node *) NULL)
			agg_list = nconc(agg_list,
					 check_having_qual_for_aggs(right, subplanTargetList,
								    groupClause));

		return agg_list;
	}
	else if (IsA(clause, Param) || IsA(clause, Const))
	{
		/* do nothing! */
		return NIL;
	}
	/* This is for Sublinks which show up as EXPR nodes. All the other EXPR nodes
         * (funcclauses, and_clauses, or_clauses) were caught above */
	else if (IsA(clause, Expr))
	  {
	    /* Only the lefthand side of the sublink has to be checked for aggregates
             * to be attached to result_plan->aggs (see planner.c:union_planner() )
	     */	    
	    foreach(t,((List *)((SubLink *)((SubPlan *)
		       ((Expr *)clause)->oper)->sublink)->lefthand)) 
	      {
		agg_list = 
		  nconc(agg_list,
			check_having_qual_for_aggs(lfirst(t), 
						   subplanTargetList, groupClause));
	      }

            /* The first argument of ...->oper has also to be checked */
	    {	      
	      List *tmp_ptr;
	      
	      foreach(tmp_ptr, ((SubLink *)((SubPlan *)
					    ((Expr *)clause)->oper)->sublink)->oper)
		{		
  		  agg_list = 
		    nconc(agg_list,
			  check_having_qual_for_aggs((Node *)lfirst(((Expr *)
								     lfirst(tmp_ptr))->args), 
						     subplanTargetList, groupClause));
		}
	    }
	      		
	    /* All arguments to the Sublink node are attributes from outside used within
	     * the sublink. Here we have to check that only attributes that is grouped for
	     * are used! */
	    foreach(t,((Expr *)clause)->args) 
	      {	
		contained_in_group_clause = 0;

		foreach(l1,groupClause)
		  {
		    if (tlist_member(lfirst(t),lcons(((GroupClause *)lfirst(l1))->entry,NIL)) != 
			NULL)
		      {
			contained_in_group_clause=1;
		      }
		  }
		
		/* If the use of the attribute is allowed (i.e. it is in the groupClause)
		 * we have to adjust the varnos and varattnos */
		if (contained_in_group_clause)
		  {
		    agg_list = 
		      nconc(agg_list,
			    check_having_qual_for_aggs(lfirst(t), 
						       subplanTargetList, groupClause));
		  }
		else
		  {
		    elog(ERROR,"You must group by the attribute used from outside!");
		    return NIL;
		  }		
	      }
	    return agg_list;
	  }
	else
	  {
	    /*
	     * Ooops! we can not handle that!
	     */
	    elog(ERROR, "check_having_qual_for_aggs: Can not handle this having_qual! %d\n",
		 nodeTag(clause));
	    return NIL;
	  }
}
/***S*H***/ /* End */
