/*-------------------------------------------------------------------------
 *
 * planner.c
 *	  The query optimizer external interface.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/planner.c,v 1.64 1999/08/22 20:14:48 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/internal.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

static List *make_subplanTargetList(Query *parse, List *tlist,
									AttrNumber **groupColIdx);
static Plan *make_groupplan(List *group_tlist, bool tuplePerGroup,
							List *groupClause, AttrNumber *grpColIdx,
							bool is_sorted, Plan *subplan);
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

	/* Initialize state for subselects */
	PlannerQueryLevel = 1;
	PlannerInitPlan = NULL;
	PlannerParamVar = NULL;
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

	set_plan_references(result_plan);

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
	bool		is_sorted = false;
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
		List	   *sub_tlist;

		/*
		 * Generate appropriate target list for subplan; may be different
		 * from tlist if grouping or aggregation is needed.
		 */
		sub_tlist = make_subplanTargetList(parse, tlist, &groupColIdx);

		/*
		 * Recursively plan the subqueries needed for inheritance
		 */
		result_plan = (Plan *) plan_inherit_queries(parse, sub_tlist,
													rt_index);

		/*
		 * Fix up outer target list.  NOTE: unlike the case for non-inherited
		 * query, we pass the unfixed tlist to subplans, which do their own
		 * fixing.  But we still want to fix the outer target list afterwards.
		 * I *think* this is correct --- doing the fix before recursing is
		 * definitely wrong, because preprocess_targetlist() will do the
		 * wrong thing if invoked twice on the same list. Maybe that is a bug?
		 * tgl 6/6/99
		 */
		tlist = preprocess_targetlist(tlist,
									  parse->commandType,
									  parse->resultRelation,
									  parse->rtable);

		if (parse->rowMark != NULL)
			elog(ERROR, "SELECT FOR UPDATE is not supported for inherit queries");
	}
	else
	{
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

				var = makeVar(rowmark->rti, -1, TIDOID, -1, 0);

				ctid = makeTargetEntry(resdom, (Node *) var);
				tlist = lappend(tlist, ctid);
			}
		}

		/*
		 * Figure out whether we need a sorted result from query_planner.
		 *
		 * If we have a GROUP BY clause, then we want a result sorted
		 * properly for grouping.  Otherwise, if there is an ORDER BY clause
		 * and no need for an aggregate node, we want to sort by the ORDER BY
		 * clause.  (XXX In some cases, we could presort even when there is
		 * an aggregate, but I'll leave that refinement for another day.)
		 *
		 * NOTE: the reason we put the target pathkeys into the Query node
		 * rather than passing them as an argument to query_planner is that
		 * the low-level routines in indxpath.c want to be able to see them.
		 */
		if (parse->groupClause)
		{
			parse->query_pathkeys =
				make_pathkeys_for_sortclauses(parse->groupClause, tlist);
		}
		else if (parse->sortClause && ! parse->hasAggs)
		{
			parse->query_pathkeys =
				make_pathkeys_for_sortclauses(parse->sortClause, tlist);
		}
		else
		{
			parse->query_pathkeys = NIL;
		}

		/*
		 * Generate appropriate target list for subplan; may be different
		 * from tlist if grouping or aggregation is needed.
		 */
		sub_tlist = make_subplanTargetList(parse, tlist, &groupColIdx);

		/* Generate the (sub) plan */
		result_plan = query_planner(parse,
									parse->commandType,
									sub_tlist,
									(List *) parse->qual);

		/* query_planner sets query_pathkeys to NIL if it didn't make
		 * a properly sorted plan
		 */
		if (parse->query_pathkeys)
			is_sorted = true;
	}

	/* query_planner returns NULL if it thinks plan is bogus */
	if (! result_plan)
		elog(ERROR, "union_planner: failed to create plan");

	/*
	 * If we have a GROUP BY clause, insert a group node (plus the
	 * appropriate sort node, if necessary).
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
		 * the same set of vars as the subplan did (but we can exclude
		 * any GROUP BY expressions).  If there are no aggregates
		 * then the Group node had better compute the final tlist.
		 */
		if (parse->hasAggs)
			group_tlist = flatten_tlist(result_plan->targetlist);
		else
			group_tlist = tlist;

		result_plan = make_groupplan(group_tlist,
									 tuplePerGroup,
									 parse->groupClause,
									 groupColIdx,
									 is_sorted,
									 result_plan);

		/*
		 * Assume the result of the group step is not ordered suitably
		 * for any ORDER BY that may exist.  XXX it might be; improve this!
		 */
		is_sorted = false;
	}

	/*
	 * If we have a HAVING clause, do the necessary things with it.
	 */
	if (parse->havingQual)
	{
		List	   *ql;

		/* convert the havingQual to conjunctive normal form (cnf) */
		parse->havingQual = (Node *) cnfify((Expr *) parse->havingQual, true);

		if (parse->hasSubLinks)
		{
			/*
			 * There may be a subselect in the havingQual, so we have to
			 * process it using the same function as for a subselect in
			 * 'where'
			 */
			parse->havingQual = SS_process_sublinks(parse->havingQual);

			/*
			 * Check for ungrouped variables passed to subplans. (Probably
			 * this should be done for the targetlist as well???)
			 */
			if (check_subplans_for_ungrouped_vars(parse->havingQual,
												  parse->groupClause,
												  parse->targetList))
				elog(ERROR, "Sub-SELECT in HAVING clause must use only GROUPed attributes from outer SELECT");
		}

		/*
		 * Require an aggregate function to appear in each clause of the
		 * havingQual (else it could have been done as a WHERE constraint).
		 */
		foreach(ql, (List *) parse->havingQual)
		{
			if (pull_agg_clause(lfirst(ql)) == NIL)
				elog(ERROR, "SELECT/HAVING requires aggregates to be valid");
		}
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
		 * Assume result is not ordered suitably for ORDER BY.
		 * XXX it might be; improve this!
		 */
		is_sorted = false;
	}

	/*
	 * If we were not able to make the plan come out in the right order,
	 * add an explicit sort step.
	 */
	if (parse->sortClause && ! is_sorted)
	{
		result_plan = make_sortplan(tlist, parse->sortClause, result_plan);
	}

	/*
	 * Finally, if there is a UNIQUE clause, add the UNIQUE node.
	 */
	if (parse->uniqueFlag)
	{
		result_plan = (Plan *) make_unique(tlist, result_plan,
										   parse->uniqueFlag);
	}

	return result_plan;
}

/*---------------
 * make_subplanTargetList
 *	  Generate appropriate target list when grouping is required.
 *
 * When union_planner inserts Aggregate and/or Group plan nodes above
 * the result of query_planner, we typically want to pass a different
 * target list to query_planner than the outer plan nodes should have.
 * This routine generates the correct target list for the subplan.
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
 *		a,b,c,d,a+b
 * where the a+b target will be used by the Sort/Group steps, and the
 * other targets will be used for computing the final results.  (In the
 * above example we could theoretically suppress the a and b targets and
 * use only a+b, but it's not really worth the trouble.)
 *
 * 'parse' is the query being processed.
 * 'tlist' is the query's target list.
 * 'groupColIdx' receives an array of column numbers for the GROUP BY
 * expressions (if there are any) in the subplan's target list.
 *
 * The result is the targetlist to be passed to the subplan.
 *---------------
 */
static List *
make_subplanTargetList(Query *parse,
					   List *tlist,
					   AttrNumber **groupColIdx)
{
	List	   *sub_tlist;
	List	   *extravars;
	int			numCols;

	*groupColIdx = NULL;

	/*
	 * If we're not grouping or aggregating, nothing to do here;
	 * query_planner should receive the unmodified target list.
	 */
	if (!parse->hasAggs && !parse->groupClause && !parse->havingQual)
		return tlist;

	/*
	 * Otherwise, start with a "flattened" tlist (having just the vars
	 * mentioned in the targetlist and HAVING qual).
	 */
	sub_tlist = flatten_tlist(tlist);
	extravars = pull_var_clause(parse->havingQual);
	sub_tlist = add_to_flat_tlist(sub_tlist, extravars);
	freeList(extravars);

	/*
	 * If grouping, create sub_tlist entries for all GROUP BY expressions
	 * (GROUP BY items that are simple Vars should be in the list already),
	 * and make an array showing where the group columns are in the sub_tlist.
	 */
	numCols = length(parse->groupClause);
	if (numCols > 0)
	{
		int			keyno = 0;
		AttrNumber *grpColIdx;
		List	   *gl;

		grpColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
		*groupColIdx = grpColIdx;

		foreach(gl, parse->groupClause)
		{
			GroupClause	   *grpcl = (GroupClause *) lfirst(gl);
			Node		   *groupexpr = get_sortgroupclause_expr(grpcl, tlist);
			TargetEntry	   *te = NULL;
			List		   *sl;

			/* Find or make a matching sub_tlist entry */
			foreach(sl, sub_tlist)
			{
				te = (TargetEntry *) lfirst(sl);
				if (equal(groupexpr, te->expr))
					break;
			}
			if (! sl)
			{
				te = makeTargetEntry(makeResdom(length(sub_tlist) + 1,
												exprType(groupexpr),
												exprTypmod(groupexpr),
												NULL,
												(Index) 0,
												(Oid) 0,
												false),
									 groupexpr);
				sub_tlist = lappend(sub_tlist, te);
			}

			/* and save its resno */
			grpColIdx[keyno++] = te->resdom->resno;
		}
	}

	return sub_tlist;
}

/*
 * make_groupplan
 *		Add a Group node for GROUP BY processing.
 *		If we couldn't make the subplan produce presorted output for grouping,
 *		first add an explicit Sort node.
 */
static Plan *
make_groupplan(List *group_tlist,
			   bool tuplePerGroup,
			   List *groupClause,
			   AttrNumber *grpColIdx,
			   bool is_sorted,
			   Plan *subplan)
{
	int			numCols = length(groupClause);

	if (! is_sorted)
	{
		/*
		 * The Sort node always just takes a copy of the subplan's tlist
		 * plus ordering information.  (This might seem inefficient if the
		 * subplan contains complex GROUP BY expressions, but in fact Sort
		 * does not evaluate its targetlist --- it only outputs the same
		 * tuples in a new order.  So the expressions we might be copying
		 * are just dummies with no extra execution cost.)
		 */
		List	   *sort_tlist = new_unsorted_tlist(subplan->targetlist);
		int			keyno = 0;
		List	   *gl;

		foreach(gl, groupClause)
		{
			GroupClause	   *grpcl = (GroupClause *) lfirst(gl);
			TargetEntry	   *te = nth(grpColIdx[keyno]-1, sort_tlist);
			Resdom		   *resdom = te->resdom;

			/*
			 * Check for the possibility of duplicate group-by clauses --- the
			 * parser should have removed 'em, but the Sort executor will get
			 * terribly confused if any get through!
			 */
			if (resdom->reskey == 0)
			{
				/* OK, insert the ordering info needed by the executor. */
				resdom->reskey = ++keyno;
				resdom->reskeyop = get_opcode(grpcl->sortop);
			}
		}

		subplan = (Plan *) make_sort(sort_tlist,
									 _NONAME_RELATION_ID_,
									 subplan,
									 keyno);
	}

	return (Plan *) make_group(group_tlist, tuplePerGroup, numCols,
							   grpColIdx, subplan);
}

/*
 * make_sortplan
 *	  Add a Sort node to implement an explicit ORDER BY clause.
 */
static Plan *
make_sortplan(List *tlist, List *sortcls, Plan *plannode)
{
	List	   *temp_tlist;
	List	   *i;
	int			keyno = 0;

	/*
	 * First make a copy of the tlist so that we don't corrupt the
	 * original.
	 */

	temp_tlist = new_unsorted_tlist(tlist);

	foreach(i, sortcls)
	{
		SortClause *sortcl = (SortClause *) lfirst(i);
		Index		refnumber = sortcl->tleSortGroupRef;
		TargetEntry *tle = NULL;
		Resdom	   *resdom;
		List	   *l;

		foreach(l, temp_tlist)
		{
			tle = (TargetEntry *) lfirst(l);
			if (tle->resdom->ressortgroupref == refnumber)
				break;
		}
		if (l == NIL)
			elog(ERROR, "make_sortplan: ORDER BY expression not found in targetlist");
		resdom = tle->resdom;

		/*
		 * Check for the possibility of duplicate order-by clauses --- the
		 * parser should have removed 'em, but the executor will get terribly
		 * confused if any get through!
		 */
		if (resdom->reskey == 0)
		{
			/* OK, insert the ordering info needed by the executor. */
			resdom->reskey = ++keyno;
			resdom->reskeyop = get_opcode(sortcl->sortop);
		}
	}

	return (Plan *) make_sort(temp_tlist,
							  _NONAME_RELATION_ID_,
							  plannode,
							  keyno);
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
