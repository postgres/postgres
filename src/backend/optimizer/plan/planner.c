/*-------------------------------------------------------------------------
 *
 * planner.c--
 *	  The query optimizer external interface.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/planner.c,v 1.14 1997/12/20 07:59:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"
#include "nodes/relation.h"
#include "parser/parse_expr.h"

#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "access/heapam.h"

#include "optimizer/internal.h"
#include "optimizer/planner.h"
#include "optimizer/plancat.h"
#include "optimizer/prep.h"
#include "optimizer/planmain.h"
#include "optimizer/paths.h"
#include "optimizer/cost.h"

/* DATA STRUCTURE CREATION/MANIPULATION ROUTINES */
#include "nodes/relation.h"
#include "optimizer/clauseinfo.h"
#include "optimizer/joininfo.h"
#include "optimizer/keys.h"
#include "optimizer/ordering.h"
#include "optimizer/pathnode.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"

#include "executor/executor.h"

static Plan *make_sortplan(List *tlist, List *sortcls, Plan *plannode);
extern Plan *make_groupPlan(List **tlist, bool tuplePerGroup,
			   List *groupClause, Plan *subplan);

/*****************************************************************************
 *
 *	   Query optimizer entry point
 *
 *****************************************************************************/


/*
 * planner--
 *	  Main query optimizer routine.
 *
 *	  Invokes the planner on union queries if there are any left,
 *	  recursing if necessary to get them all, then processes normal plans.
 *
 * Returns a query plan.
 *
 */
Plan	   *
planner(Query *parse)
{
	List	   *tlist = parse->targetList;
	List	   *rangetable = parse->rtable;
	char	   *uniqueflag = parse->uniqueFlag;
	List	   *sortclause = parse->sortClause;
	Agg		   *aggplan = NULL;

	Plan	   *result_plan = (Plan *) NULL;

	List	   *primary_qual;
	int			rt_index;
	

	/*
	 * plan inheritance
	 */
	rt_index = first_matching_rt_entry(rangetable, INHERITS_FLAG);
	if (rt_index != -1)
	{
		result_plan = (Plan *) plan_union_queries((Index) rt_index,
													parse,
													INHERITS_FLAG);
		/* XXX do we need to do this? bjm 12/19/97 */
		tlist = preprocess_targetlist(tlist,
									  parse->commandType,
									  parse->resultRelation,
									  parse->rtable);
	}
	else
	{
		tlist = preprocess_targetlist(tlist,
									  parse->commandType,
									  parse->resultRelation,
									  parse->rtable);

		primary_qual = cnfify((Expr *) parse->qual, true);

		result_plan = query_planner(parse,
									  parse->commandType,
									  tlist,
									  primary_qual);
	}
	
	/*
	 * If we have a GROUP BY clause, insert a group node (with the
	 * appropriate sort node.)
	 */
	if (parse->groupClause != NULL)
	{
		bool		tuplePerGroup;

		/*
		 * decide whether how many tuples per group the Group node needs
		 * to return. (Needs only one tuple per group if no aggregate is
		 * present. Otherwise, need every tuple from the group to do the
		 * aggregation.)
		 */
		tuplePerGroup = (parse->qry_aggs) ? TRUE : FALSE;

		result_plan =
			make_groupPlan( &tlist,
							tuplePerGroup,
							parse->groupClause,
							result_plan);

	}

	/*
	 * If aggregate is present, insert the agg node
	 */
	if (parse->qry_aggs)
	{
		aggplan = make_agg(tlist,
							parse->qry_numAgg,
							parse->qry_aggs,
							result_plan);

		/*
		 * set the varno/attno entries to the appropriate references to
		 * the result tuple of the subplans. (We need to set those in the
		 * array of aggreg's in the Agg node also. Even though they're
		 * pointers, after a few dozen's of copying, they're not the same
		 * as those in the target list.)
		 */
		set_agg_tlist_references(aggplan);
		set_agg_agglist_references(aggplan);

		result_plan = (Plan *) aggplan;
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
	if (parse->groupClause == NULL && aggplan == NULL)
	{
		result_plan->targetlist = flatten_tlist_vars(tlist,
												 result_plan->targetlist);
	}

	/*
	 * For now, before we hand back the plan, check to see if there is a
	 * user-specified sort that needs to be done.  Eventually, this will
	 * be moved into the guts of the planner s.t. user specified sorts
	 * will be considered as part of the planning process. Since we can
	 * only make use of user-specified sorts in special cases, we can do
	 * the optimization step later.
	 */

	if (uniqueflag)
	{
		Plan	   *sortplan = make_sortplan(tlist, sortclause, result_plan);

		return ((Plan *) make_unique(tlist, sortplan, uniqueflag));
	}
	else
	{
		if (sortclause)
			return (make_sortplan(tlist, sortclause, result_plan));
		else
			return ((Plan *) result_plan);
	}

}

/*
 * make_sortplan--
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
								  _TEMP_RELATION_ID_,
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

	return (sortplan);
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
			elog(WARN, "return type mismatch in function decl: final query is a catalog utility");
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
			elog(WARN,
				 "function declared with no return type, but final query is a retrieve");
		else
			return;
	}

	/* by here, the function is declared to return some type */
	if ((typ = typeidType(rettype)) == NULL)
		elog(WARN, "can't find return type %d for function\n", rettype);

	/*
	 * test 3:	if the function is declared to return a value, then the
	 * final query had better be a retrieve.
	 */
	if (cmd != CMD_SELECT)
		elog(WARN, "function declared to return type %s, but final query is not a retrieve", typeTypeName(typ));

	/*
	 * test 4:	for base type returns, the target list should have exactly
	 * one entry, and its type should agree with what the user declared.
	 */

	if (typeTypeRelid(typ) == InvalidOid)
	{
		if (exec_tlist_length(tlist) > 1)
			elog(WARN, "function declared to return %s returns multiple values in final retrieve", typeTypeName(typ));

		resnode = (Resdom *) ((TargetEntry *) lfirst(tlist))->resdom;
		if (resnode->restype != rettype)
			elog(WARN, "return type mismatch in function: declared to return %s, returns %s", typeTypeName(typ), typeidTypeName(resnode->restype));

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
	if (exec_tlist_length(tlist) == 1)
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
		elog(WARN, "cannot open relation relid %d", typeTypeRelid(typ));

	relid = reln->rd_id;
	relnatts = reln->rd_rel->relnatts;

	if (exec_tlist_length(tlist) != relnatts)
		elog(WARN, "function declared to return type %s does not retrieve (%s.*)", typeTypeName(typ), typeTypeName(typ));

	/* expect attributes 1 .. n in order */
	for (i = 1; i <= relnatts; i++)
	{
		TargetEntry *tle = lfirst(tlist);
		Node	   *thenode = tle->expr;

		tlist = lnext(tlist);
		tletype = exprType(thenode);

#if 0							/* fix me */
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
				elog(WARN, "function declared to return type %s does not retrieve (%s.all)", typeTypeName(typ), typeTypeName(typ));
		}
		else
			elog(WARN, "function declared to return type %s does not retrieve (%s.all)", typeTypeName(typ), typeTypeName(typ));
#endif
		/* reach right in there, why don't you? */
		if (tletype != reln->rd_att->attrs[i - 1]->atttypid)
			elog(WARN, "function declared to return type %s does not retrieve (%s.all)", typeTypeName(typ), typeTypeName(typ));
	}

	heap_close(reln);

	/* success */
	return;
}
