/*-------------------------------------------------------------------------
 *
 * planner.c
 *	  The query optimizer external interface.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/planner.c,v 1.47 1999/04/19 01:43:11 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>
#include <string.h>

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"
#include "nodes/relation.h"
#include "nodes/makefuncs.h"
#include "catalog/pg_type.h"
#include "parser/parse_expr.h"

#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "access/heapam.h"

#include "optimizer/internal.h"
#include "optimizer/planner.h"
#include "optimizer/plancat.h"
#include "optimizer/prep.h"
#include "optimizer/planmain.h"
#include "optimizer/subselect.h"
#include "optimizer/paths.h"
#include "optimizer/cost.h"

/* DATA STRUCTURE CREATION/MANIPULATION ROUTINES */
#include "nodes/relation.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/joininfo.h"
#include "optimizer/keys.h"
#include "optimizer/ordering.h"
#include "optimizer/pathnode.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"

#include "executor/executor.h"

#include "utils/builtins.h"
#include "utils/syscache.h"
#include "access/genam.h"
#include "parser/parse_oper.h"

static bool need_sortplan(List *sortcls, Plan *plan);
static Plan *make_sortplan(List *tlist, List *sortcls, Plan *plannode);
extern Plan *make_groupPlan(List **tlist, bool tuplePerGroup,
			   List *groupClause, Plan *subplan);

/*****************************************************************************
 *
 *	   Query optimizer entry point
 *
 *****************************************************************************/
Plan *
planner(Query *parse)
{
	Plan	   *result_plan;

	PlannerQueryLevel = 1;
	PlannerVarParam = NULL;
	PlannerParamVar = NULL;
	PlannerInitPlan = NULL;
	PlannerPlanId = 0;

	transformKeySetQuery(parse);
	result_plan = union_planner(parse);

	Assert(PlannerQueryLevel == 1);
	if (PlannerPlanId > 0)
	{
		result_plan->initPlan = PlannerInitPlan;
		(void) SS_finalize_plan(result_plan);
	}
	result_plan->nParamExec = length(PlannerParamVar);

	return result_plan;
}

/*
 * union_planner
 *
 *	  Invokes the planner on union queries if there are any left,
 *	  recursing if necessary to get them all, then processes normal plans.
 *
 * Returns a query plan.
 *
 */
Plan *
union_planner(Query *parse)
{
	List	   *tlist = parse->targetList;

	/***S*H***/
	/* copy the original tlist, we will need the original one 
	 * for the AGG node later on */
	List    *new_tlist = new_unsorted_tlist(tlist);
		
	List	   *rangetable = parse->rtable;

	Plan	   *result_plan = (Plan *) NULL;

	Index		rt_index;

	if (parse->unionClause)
	{
	  result_plan = (Plan *) plan_union_queries(parse);
	  /* XXX do we need to do this? bjm 12/19/97 */	  	  
	  tlist = preprocess_targetlist(tlist,
					parse->commandType,
					parse->resultRelation,
					parse->rtable);
	}
	else if ((rt_index = first_inherit_rt_entry(rangetable)) != -1)
	{
		if (parse->rowMark != NULL)
			elog(ERROR, "SELECT FOR UPDATE is not supported for inherit queries");
		result_plan = (Plan *) plan_inherit_queries(parse, rt_index);
		/* XXX do we need to do this? bjm 12/19/97 */
		tlist = preprocess_targetlist(tlist,
					      parse->commandType,
					      parse->resultRelation,
					      parse->rtable);
	}
	else
	{
	  List  **vpm = NULL;

	  /*
	   * If there is a HAVING clause, make sure all vars referenced in it
	   * are included in the target list for query_planner().
	   */
	  if (parse->havingQual)
		  new_tlist = check_having_qual_for_vars(parse->havingQual, new_tlist);

	  new_tlist = preprocess_targetlist(new_tlist,
					    parse->commandType,
					    parse->resultRelation,
					    parse->rtable);

	  /* FOR UPDATE ... */
	  if (parse->rowMark != NULL)
	  {
	  	List		   *l;
		TargetEntry	   *ctid;
		Resdom		   *resdom;
		Var			   *var;
		char		   *resname;

		foreach (l, parse->rowMark)
		{
			if (!(((RowMark*)lfirst(l))->info & ROW_MARK_FOR_UPDATE))
				continue;

			resname = (char*) palloc(32);
			sprintf(resname, "ctid%u", ((RowMark*)lfirst(l))->rti);
			resdom = makeResdom(length(new_tlist) + 1,
								TIDOID,
								-1,
								resname,
								0,
								0,
								1);

			var = makeVar(((RowMark*)lfirst(l))->rti, -1, TIDOID, 
							-1, 0, ((RowMark*)lfirst(l))->rti, -1);

			ctid = makeTargetEntry(resdom, (Node *) var);
			new_tlist = lappend(new_tlist, ctid);
		}
	  }
	  
	  /* Here starts the original (pre having) code */
	  tlist = preprocess_targetlist(tlist,
					parse->commandType,
					parse->resultRelation,
					parse->rtable);
	  
	  if (parse->rtable != NULL)
	  {
	      vpm = (List **) palloc(length(parse->rtable) * sizeof(List *));
	      memset(vpm, 0, length(parse->rtable) * sizeof(List *));
	  }
	  PlannerVarParam = lcons(vpm, PlannerVarParam);
	  result_plan = query_planner(parse,
				      parse->commandType,
				      new_tlist,
				      (List *) parse->qual);
	  PlannerVarParam = lnext(PlannerVarParam);
	  if (vpm != NULL)
	    pfree(vpm);		 
	}
	
	/*
	 * If we have a GROUP BY clause, insert a group node (with the
	 * appropriate sort node.)
	 */
	if (parse->groupClause)
	{
		bool		tuplePerGroup;

		/*
		 * decide whether how many tuples per group the Group node needs
		 * to return. (Needs only one tuple per group if no aggregate is
		 * present. Otherwise, need every tuple from the group to do the
		 * aggregation.)
		 */
		tuplePerGroup = parse->hasAggs;

		/***S*H***/
		/* Use 'new_tlist' instead of 'tlist' */
		result_plan = make_groupPlan(&new_tlist,
								   tuplePerGroup,
								   parse->groupClause,
								   result_plan);
	}

	/*
	 * If we have a HAVING clause, do the necessary things with it.
	 */
	if (parse->havingQual)
	{
		List  **vpm = NULL;

		if (parse->rtable != NULL)
		{
			vpm = (List **) palloc(length(parse->rtable) * sizeof(List *));
			memset(vpm, 0, length(parse->rtable) * sizeof(List *));
		}
		PlannerVarParam = lcons(vpm, PlannerVarParam);

		/* convert the havingQual to conjunctive normal form (cnf) */
		parse->havingQual = (Node *) cnfify((Expr *) parse->havingQual, true);

		if (parse->hasSubLinks)
		{
			/* There is a subselect in the havingQual, so we have to process it
			 * using the same function as for a subselect in 'where'
			 */
			parse->havingQual =
				(Node *) SS_process_sublinks(parse->havingQual);
			/* Check for ungrouped variables passed to subplans.
			 * (Probably this should be done by the parser, but right now
			 * the parser is not smart enough to tell which level the vars
			 * belong to?)
			 */
			check_having_for_ungrouped_vars(parse->havingQual,
											parse->groupClause);
		}

		/* Calculate the opfids from the opnos */
		parse->havingQual = (Node *) fix_opids((List *) parse->havingQual);

		PlannerVarParam = lnext(PlannerVarParam);
		if (vpm != NULL)
			pfree(vpm);		
	}

	/*
	 * If aggregate is present, insert the agg node
	 */
	if (parse->hasAggs)
	{
		/* Use 'tlist' not 'new_tlist' as target list because we
		 * don't want the additional attributes used for the havingQual
		 * (see above) to show up in the result
		 */
		result_plan = (Plan *) make_agg(tlist, result_plan);

		/* HAVING clause, if any, becomes qual of the Agg node */
		result_plan->qual = (List *) parse->havingQual;

		/*
		 * Update vars to refer to subplan result tuples,
		 * find Aggrefs, make sure there is an Aggref in every HAVING clause.
		 */
		if (! set_agg_tlist_references((Agg *) result_plan))
			elog(ERROR, "SELECT/HAVING requires aggregates to be valid");
	}		  

	/*
	 * For now, before we hand back the plan, check to see if there is a
	 * user-specified sort that needs to be done.  Eventually, this will
	 * be moved into the guts of the planner s.t. user specified sorts
	 * will be considered as part of the planning process. Since we can
	 * only make use of user-specified sorts in special cases, we can do
	 * the optimization step later.
	 */

	if (parse->uniqueFlag)
	{
		Plan	   *sortplan = make_sortplan(tlist, parse->sortClause, result_plan);

		return ((Plan *) make_unique(tlist, sortplan, parse->uniqueFlag));
	}
	else
	{
		if (parse->sortClause && need_sortplan(parse->sortClause, result_plan))
			return (make_sortplan(tlist, parse->sortClause, result_plan));
		else
			return ((Plan *) result_plan);
	}

}

/*
 * make_sortplan
 *	  Returns a sortplan which is basically a SORT node attached to the
 *	  top of the plan returned from the planner.  It also adds the
 *	   cost of sorting into the plan.
 *
 * sortkeys: ( resdom1 resdom2 resdom3 ...)
 * sortops:  (sortop1 sortop2 sortop3 ...)
 */
static Plan *
make_sortplan(List *tlist, List *sortcls, Plan *plannode)
{
	Plan	   *sortplan = (Plan *) NULL;
	List	   *temp_tlist = NIL;
	List	   *i = NIL;
	Resdom	   *resnode = (Resdom *) NULL;
	Resdom	   *resdom = (Resdom *) NULL;
	int			keyno = 1;

	/*
	 * First make a copy of the tlist so that we don't corrupt the the
	 * original .
	 */

	temp_tlist = new_unsorted_tlist(tlist);

	foreach(i, sortcls)
	{
		SortClause *sortcl = (SortClause *) lfirst(i);

		resnode = sortcl->resdom;
		resdom = tlist_resdom(temp_tlist, resnode);

		/*
		 * Order the resdom keys and replace the operator OID for each key
		 * with the regproc OID.
		 */
		resdom->reskey = keyno;
		resdom->reskeyop = get_opcode(sortcl->opoid);
		keyno += 1;
	}

	sortplan = (Plan *) make_sort(temp_tlist,
								  _NONAME_RELATION_ID_,
								  (Plan *) plannode,
								  length(sortcls));

	/*
	 * XXX Assuming that an internal sort has no. cost. This is wrong, but
	 * given that at this point, we don't know the no. of tuples returned,
	 * etc, we can't do better than to add a constant cost. This will be
	 * fixed once we move the sort further into the planner, but for now
	 * ... functionality....
	 */

	sortplan->cost = plannode->cost;

	return sortplan;
}

/*
 * pg_checkretval() -- check return value of a list of sql parse
 *						trees.
 *
 * The return value of a sql function is the value returned by
 * the final query in the function.  We do some ad-hoc define-time
 * type checking here to be sure that the user is returning the
 * type he claims.
 */
void
pg_checkretval(Oid rettype, QueryTreeList *queryTreeList)
{
	Query	   *parse;
	List	   *tlist;
	List	   *rt;
	int			cmd;
	Type		typ;
	Resdom	   *resnode;
	Relation	reln;
	Oid			relid;
	Oid			tletype;
	int			relnatts;
	int			i;

	/* find the final query */
	parse = queryTreeList->qtrees[queryTreeList->len - 1];

	/*
	 * test 1:	if the last query is a utility invocation, then there had
	 * better not be a return value declared.
	 */
	if (parse->commandType == CMD_UTILITY)
	{
		if (rettype == InvalidOid)
			return;
		else
			elog(ERROR, "return type mismatch in function decl: final query is a catalog utility");
	}

	/* okay, it's an ordinary query */
	tlist = parse->targetList;
	rt = parse->rtable;
	cmd = parse->commandType;

	/*
	 * test 2:	if the function is declared to return no value, then the
	 * final query had better not be a retrieve.
	 */
	if (rettype == InvalidOid)
	{
		if (cmd == CMD_SELECT)
			elog(ERROR,
				 "function declared with no return type, but final query is a retrieve");
		else
			return;
	}

	/* by here, the function is declared to return some type */
	if ((typ = typeidType(rettype)) == NULL)
		elog(ERROR, "can't find return type %d for function\n", rettype);

	/*
	 * test 3:	if the function is declared to return a value, then the
	 * final query had better be a retrieve.
	 */
	if (cmd != CMD_SELECT)
		elog(ERROR, "function declared to return type %s, but final query is not a retrieve", typeTypeName(typ));

	/*
	 * test 4:	for base type returns, the target list should have exactly
	 * one entry, and its type should agree with what the user declared.
	 */

	if (typeTypeRelid(typ) == InvalidOid)
	{
		if (ExecTargetListLength(tlist) > 1)
			elog(ERROR, "function declared to return %s returns multiple values in final retrieve", typeTypeName(typ));

		resnode = (Resdom *) ((TargetEntry *) lfirst(tlist))->resdom;
		if (resnode->restype != rettype)
			elog(ERROR, "return type mismatch in function: declared to return %s, returns %s", typeTypeName(typ), typeidTypeName(resnode->restype));

		/* by here, base return types match */
		return;
	}

	/*
	 * If the target list is of length 1, and the type of the varnode in
	 * the target list is the same as the declared return type, this is
	 * okay.  This can happen, for example, where the body of the function
	 * is 'retrieve (x = func2())', where func2 has the same return type
	 * as the function that's calling it.
	 */
	if (ExecTargetListLength(tlist) == 1)
	{
		resnode = (Resdom *) ((TargetEntry *) lfirst(tlist))->resdom;
		if (resnode->restype == rettype)
			return;
	}

	/*
	 * By here, the procedure returns a (set of) tuples.  This part of the
	 * typechecking is a hack.	We look up the relation that is the
	 * declared return type, and be sure that attributes 1 .. n in the
	 * target list match the declared types.
	 */
	reln = heap_open(typeTypeRelid(typ));

	if (!RelationIsValid(reln))
		elog(ERROR, "cannot open relation relid %d", typeTypeRelid(typ));

	relid = reln->rd_id;
	relnatts = reln->rd_rel->relnatts;

	if (ExecTargetListLength(tlist) != relnatts)
		elog(ERROR, "function declared to return type %s does not retrieve (%s.*)", typeTypeName(typ), typeTypeName(typ));

	/* expect attributes 1 .. n in order */
	for (i = 1; i <= relnatts; i++)
	{
		TargetEntry *tle = lfirst(tlist);
		Node	   *thenode = tle->expr;

		tlist = lnext(tlist);
		tletype = exprType(thenode);

#ifdef NOT_USED						/* fix me */
		/* this is tedious */
		if (IsA(thenode, Var))
			tletype = (Oid) ((Var *) thenode)->vartype;
		else if (IsA(thenode, Const))
			tletype = (Oid) ((Const *) thenode)->consttype;
		else if (IsA(thenode, Param))
			tletype = (Oid) ((Param *) thenode)->paramtype;
		else if (IsA(thenode, Expr))
			tletype = Expr;

		else if (IsA(thenode, LispList))
		{
			thenode = lfirst(thenode);
			if (IsA(thenode, Oper))
				tletype = (Oid) get_opresulttype((Oper *) thenode);
			else if (IsA(thenode, Func))
				tletype = (Oid) get_functype((Func *) thenode);
			else
				elog(ERROR, "function declared to return type %s does not retrieve (%s.all)", typeTypeName(typ), typeTypeName(typ));
		}
		else
			elog(ERROR, "function declared to return type %s does not retrieve (%s.all)", typeTypeName(typ), typeTypeName(typ));
#endif
		/* reach right in there, why don't you? */
		if (tletype != reln->rd_att->attrs[i - 1]->atttypid)
			elog(ERROR, "function declared to return type %s does not retrieve (%s.all)", typeTypeName(typ), typeTypeName(typ));
	}

	heap_close(reln);

	/* success */
	return;
}


/* ----------
 * Support function for need_sortplan
 * ----------
 */
static TargetEntry *
get_matching_tle(Plan *plan, Resdom *resdom)
{
	List		*i;
	TargetEntry	*tle;

	foreach (i, plan->targetlist) {
		tle = (TargetEntry *)lfirst(i);
		if (tle->resdom->resno == resdom->resno)
			return tle;
	}
	return NULL;
}


/* ----------
 * Check if a user requested ORDER BY is already satisfied by
 * the choosen index scan.
 *
 * Returns TRUE if sort is required, FALSE if can be omitted.
 * ----------
 */
static bool
need_sortplan(List *sortcls, Plan *plan)
{
	Relation	indexRel;
	IndexScan	*indexScan;
	Oid		indexId;
	List		*i;
	HeapTuple	htup;
	Form_pg_index	index_tup;
	int		key_no = 0;

	/* ----------
	 * Must be an IndexScan
	 * ----------
	 */
	if (nodeTag(plan) != T_IndexScan) {
		return TRUE;
	}

	indexScan = (IndexScan *)plan;

	/* ----------
	 * Should not have left- or righttree
	 * ----------
	 */
	if (plan->lefttree != NULL) {
		return TRUE;
	}
	if (plan->righttree != NULL) {
		return TRUE;
	}

	/* ----------
	 * Must be a single index scan
	 * ----------
	 */
	if (length(indexScan->indxid) != 1) {
		return TRUE;
	}

	/* ----------
	 * Indices can only have up to 8 attributes. So an ORDER BY using
	 * more that 8 attributes could never be satisfied by an index.
	 * ----------
	 */
	if (length(sortcls) > 8) {
		return TRUE;
	}

	/* ----------
	 * The choosen Index must be a btree
	 * ----------
	 */
	indexId = lfirsti(indexScan->indxid);

	indexRel = index_open(indexId);
	if (strcmp(nameout(&(indexRel->rd_am->amname)), "btree") != 0) {
		heap_close(indexRel);
		return TRUE;
	}
	heap_close(indexRel);

	/* ----------
	 * Fetch the index tuple
	 * ----------
	 */
	htup = SearchSysCacheTuple(INDEXRELID,
			ObjectIdGetDatum(indexId), 0, 0, 0);
	if (!HeapTupleIsValid(htup)) {
		elog(ERROR, "cache lookup for index %d failed", indexId);
	}
	index_tup = (Form_pg_index) GETSTRUCT(htup);

	/* ----------
	 * Check if all the sort clauses match the attributes in the index
	 * ----------
	 */
	foreach (i, sortcls) {
		SortClause	*sortcl;
		Resdom		*resdom;
		TargetEntry	*tle;
		Var		*var;

		sortcl = (SortClause *) lfirst(i);

		resdom = sortcl->resdom;
		tle = get_matching_tle(plan, resdom);
		if (tle == NULL) {
			/* ----------
			 * Could this happen?
			 * ----------
			 */
			return TRUE;
		}
		if (nodeTag(tle->expr) != T_Var) {
			/* ----------
			 * The target list expression isn't a var, so it
			 * cannot be the indexed attribute
			 * ----------
			 */
			return TRUE;
		}
		var = (Var *)(tle->expr);

		if (var->varno != indexScan->scan.scanrelid) {
			/* ----------
			 * This Var isn't from the scan relation. So it isn't
			 * that of the index
			 * ----------
			 */
			return TRUE;
		}

		if (var->varattno != index_tup->indkey[key_no]) {
			/* ----------
			 * It isn't the indexed attribute.
			 * ----------
			 */
			return TRUE;
		}

		if (oprid(oper("<", resdom->restype, resdom->restype, FALSE)) != sortcl->opoid) {
			/* ----------
			 * Sort order isn't in ascending order.
			 * ----------
			 */
			return TRUE;
		}

		key_no++;
	}

	/* ----------
	 * Index matches ORDER BY - sort not required
	 * ----------
	 */
	return FALSE;
}

