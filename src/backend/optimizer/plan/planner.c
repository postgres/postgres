/*-------------------------------------------------------------------------
 *
 * planner.c
 *	  The query optimizer external interface.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/planner.c,v 1.54 1999/05/25 16:09:37 momjian Exp $
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

static List *make_subplanTargetList(Query *parse, List *tlist,
					   AttrNumber **groupColIdx);
static Plan *make_groupplan(List *group_tlist, bool tuplePerGroup,
			   List *groupClause, AttrNumber *grpColIdx,
			   Plan *subplan);
static bool need_sortplan(List *sortcls, Plan *plan);
static Plan *make_sortplan(List *tlist, List *sortcls, Plan *plannode);

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
	List	   *rangetable = parse->rtable;
	Plan	   *result_plan = (Plan *) NULL;
	AttrNumber *groupColIdx = NULL;
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
		List	  **vpm = NULL;
		List	   *sub_tlist;

		/* Preprocess targetlist in case we are inside an INSERT/UPDATE. */
		tlist = preprocess_targetlist(tlist,
									  parse->commandType,
									  parse->resultRelation,
									  parse->rtable);

		/*
		 * Add row-mark targets for UPDATE (should this be done in
		 * preprocess_targetlist?)
		 */
		if (parse->rowMark != NULL)
		{
			List	   *l;

			foreach(l, parse->rowMark)
			{
				RowMark    *rowmark = (RowMark *) lfirst(l);
				TargetEntry *ctid;
				Resdom	   *resdom;
				Var		   *var;
				char	   *resname;

				if (!(rowmark->info & ROW_MARK_FOR_UPDATE))
					continue;

				resname = (char *) palloc(32);
				sprintf(resname, "ctid%u", rowmark->rti);
				resdom = makeResdom(length(tlist) + 1,
									TIDOID,
									-1,
									resname,
									0,
									0,
									true);

				var = makeVar(rowmark->rti, -1, TIDOID,
							  -1, 0, rowmark->rti, -1);

				ctid = makeTargetEntry(resdom, (Node *) var);
				tlist = lappend(tlist, ctid);
			}
		}

		/*
		 * Generate appropriate target list for subplan; may be different
		 * from tlist if grouping or aggregation is needed.
		 */
		sub_tlist = make_subplanTargetList(parse, tlist, &groupColIdx);

		/* Generate the (sub) plan */
		if (parse->rtable != NULL)
		{
			vpm = (List **) palloc(length(parse->rtable) * sizeof(List *));
			memset(vpm, 0, length(parse->rtable) * sizeof(List *));
		}
		PlannerVarParam = lcons(vpm, PlannerVarParam);
		result_plan = query_planner(parse,
									parse->commandType,
									sub_tlist,
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
		List	   *group_tlist;

		/*
		 * Decide whether how many tuples per group the Group node needs
		 * to return. (Needs only one tuple per group if no aggregate is
		 * present. Otherwise, need every tuple from the group to do the
		 * aggregation.)  Note tuplePerGroup is named backwards :-(
		 */
		tuplePerGroup = parse->hasAggs;

		/*
		 * If there are aggregates then the Group node should just return
		 * the same (simplified) tlist as the subplan, which we indicate
		 * to make_groupplan by passing NIL.  If there are no aggregates
		 * then the Group node had better compute the final tlist.
		 */
		group_tlist = parse->hasAggs ? NIL : tlist;

		result_plan = make_groupplan(group_tlist,
									 tuplePerGroup,
									 parse->groupClause,
									 groupColIdx,
									 result_plan);
	}

	/*
	 * If we have a HAVING clause, do the necessary things with it.
	 */
	if (parse->havingQual)
	{
		List	  **vpm = NULL;

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

			/*
			 * There is a subselect in the havingQual, so we have to
			 * process it using the same function as for a subselect in
			 * 'where'
			 */
			parse->havingQual =
				(Node *) SS_process_sublinks(parse->havingQual);

			/*
			 * Check for ungrouped variables passed to subplans. (Probably
			 * this should be done by the parser, but right now the parser
			 * is not smart enough to tell which level the vars belong
			 * to?)
			 */
			check_having_for_ungrouped_vars(parse->havingQual,
											parse->groupClause,
											parse->targetList);
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
		result_plan = (Plan *) make_agg(tlist, result_plan);

		/* HAVING clause, if any, becomes qual of the Agg node */
		result_plan->qual = (List *) parse->havingQual;

		/*
		 * Update vars to refer to subplan result tuples, find Aggrefs,
		 * make sure there is an Aggref in every HAVING clause.
		 */
		if (!set_agg_tlist_references((Agg *) result_plan))
			elog(ERROR, "SELECT/HAVING requires aggregates to be valid");

		/*
		 * Check that we actually found some aggregates, else executor
		 * will die unpleasantly.  (The rewrite module currently has bugs
		 * that allow hasAggs to be incorrectly set 'true' sometimes. It's
		 * not easy to recover here, since we've already made decisions
		 * assuming there will be an Agg node.)
		 */
		if (((Agg *) result_plan)->aggs == NIL)
			elog(ERROR, "union_planner: query is marked hasAggs, but I don't see any");
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

/*---------------
 * make_subplanTargetList
 *	  Generate appropriate target lists when grouping is required.
 *
 * When union_planner inserts Aggregate and/or Group/Sort plan nodes above
 * the result of query_planner, we typically need to pass a different
 * target list to query_planner than the outer plan nodes should have.
 * This routine generates the correct target list for the subplan, and
 * if necessary modifies the target list for the inserted nodes as well.
 *
 * The initial target list passed from the parser already contains entries
 * for all ORDER BY and GROUP BY expressions, but it will not have entries
 * for variables used only in HAVING clauses; so we need to add those
 * variables to the subplan target list.  Also, if we are doing either
 * grouping or aggregation, we flatten all expressions except GROUP BY items
 * into their component variables; the other expressions will be computed by
 * the inserted nodes rather than by the subplan.  For example,
 * given a query like
 *		SELECT a+b,SUM(c+d) FROM table GROUP BY a+b;
 * we want to pass this targetlist to the subplan:
 *		a+b,c,d
 * where the a+b target will be used by the Sort/Group steps, and the
 * c and d targets will be needed to compute the aggregate results.
 *
 * 'parse' is the query being processed.
 * 'tlist' is the query's target list.  CAUTION: list elements may be
 * modified by this routine!
 * 'groupColIdx' receives an array of column numbers for the GROUP BY
 * expressions (if there are any) in the subplan's target list.
 *
 * The result is the targetlist to be passed to the subplan.  Also,
 * the parent tlist is modified so that any nontrivial targetlist items that
 * exactly match GROUP BY items are replaced by simple Var nodes referencing
 * those outputs of the subplan.  This avoids redundant recalculations in
 * cases like
 *		SELECT a+1, ... GROUP BY a+1
 * Note, however, that other varnodes in the parent's targetlist (and
 * havingQual, if any) will still need to be updated to refer to outputs
 * of the subplan.	This routine is quite large enough already, so we do
 * that later.
 *---------------
 */
static List *
make_subplanTargetList(Query *parse,
					   List *tlist,
					   AttrNumber **groupColIdx)
{
	List	   *sub_tlist;
	List	   *prnt_tlist;
	List	   *sl,
			   *gl;
	List	   *glc = NIL;
	List	   *extravars = NIL;
	int			numCols;
	AttrNumber *grpColIdx = NULL;
	int			next_resno = 1;

	*groupColIdx = NULL;

	/*
	 * If we're not grouping or aggregating, nothing to do here;
	 * query_planner should receive the unmodified target list.
	 */
	if (!parse->hasAggs && !parse->groupClause && !parse->havingQual)
		return tlist;

	/*
	 * If grouping, make a working copy of groupClause list (which we use
	 * just to verify that we found all the groupClause items in tlist).
	 * Also allocate space to remember where the group columns are in the
	 * subplan tlist.
	 */
	numCols = length(parse->groupClause);
	if (numCols > 0)
	{
		glc = listCopy(parse->groupClause);
		grpColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
		*groupColIdx = grpColIdx;
	}

	sub_tlist = new_unsorted_tlist(tlist);		/* make a modifiable copy */

	/*
	 * Step 1: build grpColIdx by finding targetlist items that match
	 * GroupBy entries.  If there are aggregates, remove non-GroupBy items
	 * from sub_tlist, and reset its resnos accordingly.  When we leave an
	 * expression in the subplan tlist, modify the parent tlist to copy
	 * the value from the subplan output rather than re-evaluating it.
	 */
	prnt_tlist = tlist;			/* scans parent tlist in sync with sl */
	foreach(sl, sub_tlist)
	{
		TargetEntry *te = (TargetEntry *) lfirst(sl);
		TargetEntry *parentte = (TargetEntry *) lfirst(prnt_tlist);
		Resdom	   *resdom = te->resdom;
		bool		keepInSubPlan = true;
		bool		foundGroupClause = false;
		int			keyno = 0;

		foreach(gl, parse->groupClause)
		{
			GroupClause *grpcl = (GroupClause *) lfirst(gl);

			keyno++;			/* sort key # for this GroupClause */
			if (grpcl->tleGroupref == resdom->resgroupref)
			{
				/* Found a matching groupclause; record info for sorting */
				foundGroupClause = true;
				resdom->reskey = keyno;
				resdom->reskeyop = get_opcode(grpcl->grpOpoid);
				grpColIdx[keyno - 1] = next_resno;

				/*
				 * Remove groupclause from our list of unmatched
				 * groupclauses. NB: this depends on having used a shallow
				 * listCopy() above.
				 */
				glc = lremove((void *) grpcl, glc);
				break;
			}
		}

		if (!foundGroupClause)
		{

			/*
			 * Non-GroupBy entry: remove it from subplan if there are
			 * aggregates in query - it will be evaluated by Aggregate
			 * plan. But do not remove simple-Var entries; we'd just have
			 * to add them back anyway, and we risk confusing
			 * INSERT/UPDATE.
			 */
			if (parse->hasAggs && !IsA(te->expr, Var))
				keepInSubPlan = false;
		}

		if (keepInSubPlan)
		{
			/* Assign new sequential resnos to subplan tlist items */
			resdom->resno = next_resno++;
			if (!IsA(parentte->expr, Var))
			{

				/*
				 * Since the item is being computed in the subplan, we can
				 * just make a Var node to reference it in the outer plan,
				 * rather than recomputing it there. Note we use varnoold
				 * = -1 as a flag to let replace_vars_with_subplan_refs
				 * know it needn't change this Var node. If it's only a
				 * Var anyway, we leave it alone for now;
				 * replace_vars_with_subplan_refs will fix it later.
				 */
				parentte->expr = (Node *) makeVar(1, resdom->resno,
												  resdom->restype,
												  resdom->restypmod,
												  0, -1, resdom->resno);
			}
		}
		else
		{

			/*
			 * Remove this tlist item from the subplan, but remember the
			 * vars it needs.  The outer tlist item probably needs
			 * changes, but that will happen later.
			 */
			sub_tlist = lremove(te, sub_tlist);
			extravars = nconc(extravars, pull_var_clause(te->expr));
		}

		prnt_tlist = lnext(prnt_tlist);
	}

	/* We should have found all the GROUP BY clauses in the tlist. */
	if (length(glc) != 0)
		elog(ERROR, "make_subplanTargetList: GROUP BY attribute not found in target list");

	/*
	 * Add subplan targets for any variables needed by removed tlist
	 * entries that aren't otherwise mentioned in the subplan target list.
	 * We'll also need targets for any variables seen only in HAVING.
	 */
	extravars = nconc(extravars, pull_var_clause(parse->havingQual));

	foreach(gl, extravars)
	{
		Var		   *v = (Var *) lfirst(gl);

		if (tlist_member(v, sub_tlist) == NULL)
		{

			/*
			 * Make sure sub_tlist element is a fresh object not shared
			 * with any other structure; not sure if anything will break
			 * if it is shared, but better to be safe...
			 */
			sub_tlist = lappend(sub_tlist,
								create_tl_element((Var *) copyObject(v),
												  next_resno));
			next_resno++;
		}
	}

	return sub_tlist;
}

static Plan *
make_groupplan(List *group_tlist,
			   bool tuplePerGroup,
			   List *groupClause,
			   AttrNumber *grpColIdx,
			   Plan *subplan)
{
	List	   *sort_tlist;
	List	   *sl;
	Sort	   *sortplan;
	Group	   *grpplan;
	int			numCols = length(groupClause);

	/*
	 * Make the targetlist for the Sort node; it always just references
	 * each of the corresponding target items of the subplan.  We need to
	 * ensure that simple Vars in the subplan's target list are
	 * recognizable by replace_vars_with_subplan_refs when it's applied to
	 * the Sort/Group target list, so copy up their varnoold/varoattno.
	 */
	sort_tlist = NIL;
	foreach(sl, subplan->targetlist)
	{
		TargetEntry *te = (TargetEntry *) lfirst(sl);
		Resdom	   *resdom = te->resdom;
		Var		   *newvar;

		if (IsA(te->expr, Var))
		{
			Var		   *subvar = (Var *) te->expr;

			newvar = makeVar(1, resdom->resno,
							 resdom->restype, resdom->restypmod,
							 0, subvar->varnoold, subvar->varoattno);
		}
		else
		{
			newvar = makeVar(1, resdom->resno,
							 resdom->restype, resdom->restypmod,
							 0, -1, resdom->resno);
		}

		sort_tlist = lappend(sort_tlist,
						   makeTargetEntry((Resdom *) copyObject(resdom),
										   (Node *) newvar));
	}

	/*
	 * Make the Sort node
	 */
	sortplan = make_sort(sort_tlist,
						 _NONAME_RELATION_ID_,
						 subplan,
						 numCols);
	sortplan->plan.cost = subplan->cost;		/* XXX assume no cost */

	/*
	 * If the caller gave us a target list, use it after fixing the
	 * variables. If not, we need the same sort of "repeater" tlist as for
	 * the Sort node.
	 */
	if (group_tlist)
	{
		group_tlist = copyObject(group_tlist);	/* necessary?? */
		replace_tlist_with_subplan_refs(group_tlist,
										(Index) 0,
										subplan->targetlist);
	}
	else
		group_tlist = copyObject(sort_tlist);

	/*
	 * Make the Group node
	 */
	grpplan = make_group(group_tlist, tuplePerGroup, numCols,
						 grpColIdx, sortplan);

	return (Plan *) grpplan;
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
 *
 * XXX Why is this function in this module?
 */
void
pg_checkretval(Oid rettype, List *queryTreeList)
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
	parse = (Query *) nth(length(queryTreeList) - 1, queryTreeList);

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
		elog(ERROR, "can't find return type %u for function\n", rettype);

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
		elog(ERROR, "cannot open relation relid %u", typeTypeRelid(typ));

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

#ifdef NOT_USED					/* fix me */
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
	List	   *i;
	TargetEntry *tle;

	foreach(i, plan->targetlist)
	{
		tle = (TargetEntry *) lfirst(i);
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
	IndexScan  *indexScan;
	Oid			indexId;
	List	   *i;
	HeapTuple	htup;
	Form_pg_index index_tup;
	int			key_no = 0;

	/* ----------
	 * Must be an IndexScan
	 * ----------
	 */
	if (nodeTag(plan) != T_IndexScan)
		return TRUE;

	indexScan = (IndexScan *) plan;

	/* ----------
	 * Should not have left- or righttree
	 * ----------
	 */
	if (plan->lefttree != NULL)
		return TRUE;
	if (plan->righttree != NULL)
		return TRUE;

	/* ----------
	 * Must be a single index scan
	 * ----------
	 */
	if (length(indexScan->indxid) != 1)
		return TRUE;

	/* ----------
	 * Indices can only have up to 8 attributes. So an ORDER BY using
	 * more that 8 attributes could never be satisfied by an index.
	 * ----------
	 */
	if (length(sortcls) > 8)
		return TRUE;

	/* ----------
	 * The choosen Index must be a btree
	 * ----------
	 */
	indexId = lfirsti(indexScan->indxid);

	indexRel = index_open(indexId);
	if (strcmp(nameout(&(indexRel->rd_am->amname)), "btree") != 0)
	{
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
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup for index %u failed", indexId);
	index_tup = (Form_pg_index) GETSTRUCT(htup);

	/* ----------
	 * Check if all the sort clauses match the attributes in the index
	 * ----------
	 */
	foreach(i, sortcls)
	{
		SortClause *sortcl;
		Resdom	   *resdom;
		TargetEntry *tle;
		Var		   *var;

		sortcl = (SortClause *) lfirst(i);

		resdom = sortcl->resdom;
		tle = get_matching_tle(plan, resdom);
		if (tle == NULL)
		{
			/* ----------
			 * Could this happen?
			 * ----------
			 */
			return TRUE;
		}
		if (nodeTag(tle->expr) != T_Var)
		{
			/* ----------
			 * The target list expression isn't a var, so it
			 * cannot be the indexed attribute
			 * ----------
			 */
			return TRUE;
		}
		var = (Var *) (tle->expr);

		if (var->varno != indexScan->scan.scanrelid)
		{
			/* ----------
			 * This Var isn't from the scan relation. So it isn't
			 * that of the index
			 * ----------
			 */
			return TRUE;
		}

		if (var->varattno != index_tup->indkey[key_no])
		{
			/* ----------
			 * It isn't the indexed attribute.
			 * ----------
			 */
			return TRUE;
		}

		if (oprid(oper("<", resdom->restype, resdom->restype, FALSE)) != sortcl->opoid)
		{
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
