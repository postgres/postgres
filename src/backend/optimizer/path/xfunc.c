/*-------------------------------------------------------------------------
 *
 * xfunc.c--
 *	  Utility routines to handle expensive function optimization.
 *	  Includes xfunc_trypullup(), which attempts early pullup of predicates
 *	  to allow for maximal pruning.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/Attic/xfunc.c,v 1.20 1998/08/24 01:37:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>				/* for MAXFLOAT on most systems */

#include <values.h>				/* for MAXFLOAT on SunOS */
#include <string.h>

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "lib/lispsort.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "nodes/relation.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/internal.h"
#include "optimizer/keys.h"
#include "optimizer/pathnode.h"
#include "optimizer/tlist.h"	/* for get_expr */
#include "optimizer/xfunc.h"
#include "storage/buf_internals.h"		/* for NBuffers */
#include "tcop/dest.h"
#include "utils/syscache.h"

#define ever ; 1 ;

/* local funcs */
static int
xfunc_card_unreferenced(Query *queryInfo,
						Expr *clause, Relid referenced);

*/

/*
** xfunc_trypullup --
**	  Preliminary pullup of predicates, to allow for maximal pruning.
** Given a relation, check each of its paths and see if you can
** pullup clauses from its inner and outer.
*/

void
xfunc_trypullup(RelOptInfo rel)
{
	LispValue	y;				/* list ptr */
	CInfo		maxcinfo;		/* The CInfo to pull up, as calculated by
								 * xfunc_shouldpull() */
	JoinPath	curpath;		/* current path in list */
	int			progress;		/* has progress been made this time
								 * through? */
	int			clausetype;

	do
	{
		progress = false;		/* no progress yet in this iteration */
		foreach(y, get_pathlist(rel))
		{
			curpath = (JoinPath) lfirst(y);

			/*
			 * * for each operand, attempt to pullup predicates until
			 * first * failure.
			 */
			for (ever)
			{
				/* No, the following should NOT be '=='  !! */
				if (clausetype =
					xfunc_shouldpull((Path) get_innerjoinpath(curpath),
									 curpath, INNER, &maxcinfo))
				{

					xfunc_pullup((Path) get_innerjoinpath(curpath),
								 curpath, maxcinfo, INNER, clausetype);
					progress = true;
				}
				else
					break;
			}
			for (ever)
			{

				/* No, the following should NOT be '=='  !! */
				if (clausetype =
					xfunc_shouldpull((Path) get_outerjoinpath(curpath),
									 curpath, OUTER, &maxcinfo))
				{

					xfunc_pullup((Path) get_outerjoinpath(curpath),
								 curpath, maxcinfo, OUTER, clausetype);
					progress = true;
				}
				else
					break;
			}

			/*
			 * * make sure the unpruneable flag bubbles up, i.e. * if
			 * anywhere below us in the path pruneable is false, * then
			 * pruneable should be false here
			 */
			if (get_pruneable(get_parent(curpath)) &&
				(!get_pruneable(get_parent
								((Path) get_innerjoinpath(curpath))) ||
				 !get_pruneable(get_parent((Path)
										   get_outerjoinpath(curpath)))))
			{

				set_pruneable(get_parent(curpath), false);
				progress = true;
			}
		}
	} while (progress);
}

/*
 ** xfunc_shouldpull --
 **    find clause with highest rank, and decide whether to pull it up
 ** from child to parent.  Currently we only pullup secondary join clauses
 ** that are in the pathclauseinfo.  Secondary hash and sort clauses are
 ** left where they are.
 **    If we find an expensive function but decide *not* to pull it up,
 ** we'd better set the unpruneable flag.  -- JMH, 11/11/92
 **
 ** Returns:  0 if nothing left to pullup
 **			  XFUNC_LOCPRD if a local predicate is to be pulled up
 **			  XFUNC_JOINPRD if a secondary join predicate is to be pulled up
 */
int
xfunc_shouldpull(Query *queryInfo,
				 Path childpath,
				 JoinPath parentpath,
				 int whichchild,
				 CInfo *maxcinfopt)		/* Out: pointer to clause to
										 * pullup */
{
	LispValue	clauselist,
				tmplist;		/* lists of clauses */
	CInfo		maxcinfo;		/* clause to pullup */
	LispValue	primjoinclause	/* primary join clause */
	= xfunc_primary_join(parentpath);
	Cost		tmprank,
				maxrank = (-1 * MAXFLOAT);		/* ranks of clauses */
	Cost		joinselec = 0;	/* selectivity of the join predicate */
	Cost		joincost = 0;	/* join cost + primjoinclause cost */
	int			retval = XFUNC_LOCPRD;

	clauselist = get_locclauseinfo(childpath);

	if (clauselist != LispNil)
	{
		/* find local predicate with maximum rank */
		for (tmplist = clauselist,
			 maxcinfo = (CInfo) lfirst(tmplist),
			 maxrank = xfunc_rank(get_clause(maxcinfo));
			 tmplist != LispNil;
			 tmplist = lnext(tmplist))
		{

			if ((tmprank = xfunc_rank(get_clause((CInfo) lfirst(tmplist))))
				> maxrank)
			{
				maxcinfo = (CInfo) lfirst(tmplist);
				maxrank = tmprank;
			}
		}
	}

	/*
	 * * If child is a join path, and there are multiple join clauses, *
	 * see if any join clause has even higher rank than the highest *
	 * local predicate
	 */
	if (is_join(childpath) && xfunc_num_join_clauses((JoinPath) childpath) > 1)
		for (tmplist = get_pathclauseinfo((JoinPath) childpath);
			 tmplist != LispNil;
			 tmplist = lnext(tmplist))
		{

			if (tmplist != LispNil &&
			  (tmprank = xfunc_rank(get_clause((CInfo) lfirst(tmplist))))
				> maxrank)
			{
				maxcinfo = (CInfo) lfirst(tmplist);
				maxrank = tmprank;
				retval = XFUNC_JOINPRD;
			}
		}
	if (maxrank == (-1 * MAXFLOAT))		/* no expensive clauses */
		return (0);

	/*
	 * * Pullup over join if clause is higher rank than join, or if * join
	 * is nested loop and current path is inner child (note that *
	 * restrictions on the inner of a nested loop don't buy you anything
	 * -- * you still have to scan the entire inner relation each time). *
	 * Note that the cost of a secondary join clause is only what's *
	 * calculated by xfunc_expense(), since the actual joining * (i.e. the
	 * usual path_cost) is paid for by the primary join clause.
	 */
	if (primjoinclause != LispNil)
	{
		joinselec = compute_clause_selec(queryInfo, primjoinclause, LispNil);
		joincost = xfunc_join_expense(parentpath, whichchild);

		if (XfuncMode == XFUNC_PULLALL ||
			(XfuncMode != XFUNC_WAIT &&
			 ((joincost != 0 &&
			   (maxrank = xfunc_rank(get_clause(maxcinfo))) >
			   ((joinselec - 1.0) / joincost))
			  || (joincost == 0 && joinselec < 1)
			  || (!is_join(childpath)
				  && (whichchild == INNER)
				  && IsA(parentpath, JoinPath)
				  &&!IsA(parentpath, HashPath)
				  &&!IsA(parentpath, MergePath)))))
		{

			*maxcinfopt = maxcinfo;
			return (retval);

		}
		else if (maxrank != -(MAXFLOAT))
		{

			/*
			 * * we've left an expensive restriction below a join.  Since *
			 * we may pullup this restriction in predmig.c, we'd best *
			 * set the RelOptInfo of this join to be unpruneable
			 */
			set_pruneable(get_parent(parentpath), false);
			/* and fall through */
		}
	}
	return (0);
}


/*
 ** xfunc_pullup --
 **    move clause from child pathnode to parent pathnode.	 This operation
 ** makes the child pathnode produce a larger relation than it used to.
 ** This means that we must construct a new RelOptInfo just for the childpath,
 ** although this RelOptInfo will not be added to the list of Rels to be joined up
 ** in the query; it's merely a parent for the new childpath.
 **    We also have to fix up the path costs of the child and parent.
 **
 ** Now returns a pointer to the new pulled-up CInfo. -- JMH, 11/18/92
 */
CInfo
xfunc_pullup(Query *queryInfo,
			 Path childpath,
			 JoinPath parentpath,
			 CInfo cinfo,		/* clause to pull up */
			 int whichchild,	/* whether child is INNER or OUTER of join */
			 int clausetype)	/* whether clause to pull is join or local */
{
	Path		newkid;
	RelOptInfo			newrel;
	Cost		pulled_selec;
	Cost		cost;
	CInfo		newinfo;

	/* remove clause from childpath */
	newkid = (Path) copyObject((Node) childpath);
	if (clausetype == XFUNC_LOCPRD)
	{
		set_locclauseinfo(newkid,
						  xfunc_LispRemove((LispValue) cinfo,
									  (List) get_locclauseinfo(newkid)));
	}
	else
	{
		set_pathclauseinfo
			((JoinPath) newkid,
			 xfunc_LispRemove((LispValue) cinfo,
						  (List) get_pathclauseinfo((JoinPath) newkid)));
	}

	/*
	 * * give the new child path its own RelOptInfo node that reflects the * lack
	 * of the pulled-up predicate
	 */
	pulled_selec = compute_clause_selec(queryInfo,
										get_clause(cinfo), LispNil);
	xfunc_copyrel(get_parent(newkid), &newrel);
	set_parent(newkid, newrel);
	set_pathlist(newrel, lcons(newkid, NIL));
	set_unorderedpath(newrel, (PathPtr) newkid);
	set_cheapestpath(newrel, (PathPtr) newkid);
	set_size(newrel,
		(Count) ((Cost) get_size(get_parent(childpath)) / pulled_selec));

	/*
	 * * fix up path cost of newkid.  To do this we subtract away all the *
	 * xfunc_costs of childpath, then recompute the xfunc_costs of newkid
	 */
	cost = get_path_cost(newkid) - xfunc_get_path_cost(childpath);
	Assert(cost >= 0);
	set_path_cost(newkid, cost);
	cost = get_path_cost(newkid) + xfunc_get_path_cost(newkid);
	set_path_cost(newkid, cost);

	/*
	 * * We copy the cinfo, since it may appear in other plans, and we're
	 * going * to munge it.  -- JMH, 7/22/92
	 */
	newinfo = (CInfo) copyObject((Node) cinfo);

	/*
	 * * Fix all vars in the clause * to point to the right varno and
	 * varattno in parentpath
	 */
	xfunc_fixvars(get_clause(newinfo), newrel, whichchild);

	/* add clause to parentpath, and fix up its cost. */
	set_locclauseinfo(parentpath,
					  lispCons((LispValue) newinfo,
							 (LispValue) get_locclauseinfo(parentpath)));
	/* put new childpath into the path tree */
	if (whichchild == INNER)
		set_innerjoinpath(parentpath, (pathPtr) newkid);
	else
		set_outerjoinpath(parentpath, (pathPtr) newkid);

	/*
	 * * recompute parentpath cost from scratch -- the cost * of the join
	 * method has changed
	 */
	cost = xfunc_total_path_cost(parentpath);
	set_path_cost(parentpath, cost);

	return (newinfo);
}

/*
 ** calculate (selectivity-1)/cost.
 */
Cost
xfunc_rank(Query *queryInfo, LispValue clause)
{
	Cost		selec = compute_clause_selec(queryInfo, clause, LispNil);
	Cost		cost = xfunc_expense(queryInfo, clause);

	if (cost == 0)
		if (selec > 1)
			return (MAXFLOAT);
		else
			return (-(MAXFLOAT));
	return ((selec - 1) / cost);
}

/*
 ** Find the "global" expense of a clause; i.e. the local expense divided
 ** by the cardinalities of all the base relations of the query that are *not*
 ** referenced in the clause.
 */
Cost
xfunc_expense(Query *queryInfo, clause)
LispValue	clause;
{
	Cost		cost = xfunc_local_expense(clause);

	if (cost)
	{
		Count		card = xfunc_card_unreferenced(queryInfo, clause, LispNil);

		if (card)
			cost /= card;
	}

	return (cost);
}

/*
 ** xfunc_join_expense --
 **    Find global expense of a join clause
 */
Cost
xfunc_join_expense(Query *queryInfo, JoinPath path, int whichchild)
{
	LispValue	primjoinclause = xfunc_primary_join(path);

	/*
	 * * the second argument to xfunc_card_unreferenced reflects all the *
	 * relations involved in the join clause, i.e. all the relids in the
	 * RelOptInfo * of the join clause
	 */
	Count		card = 0;
	Cost		cost = xfunc_expense_per_tuple(path, whichchild);

	card = xfunc_card_unreferenced(queryInfo,
								   primjoinclause,
								   get_relids(get_parent(path)));
	if (primjoinclause)
		cost += xfunc_local_expense(primjoinclause);

	if (card)
		cost /= card;

	return (cost);
}

/*
 ** Recursively find the per-tuple expense of a clause.  See
 ** xfunc_func_expense for more discussion.
 */
Cost
xfunc_local_expense(LispValue clause)
{
	Cost		cost = 0;		/* running expense */
	LispValue	tmpclause;

	/* First handle the base case */
	if (IsA(clause, Const) ||IsA(clause, Var) ||IsA(clause, Param))
		return (0);
	/* now other stuff */
	else if (IsA(clause, Iter))
		/* Too low. Should multiply by the expected number of iterations. */
		return (xfunc_local_expense(get_iterexpr((Iter) clause)));
	else if (IsA(clause, ArrayRef))
		return (xfunc_local_expense(get_refexpr((ArrayRef) clause)));
	else if (fast_is_clause(clause))
		return (xfunc_func_expense((LispValue) get_op(clause),
								   (LispValue) get_opargs(clause)));
	else if (fast_is_funcclause(clause))
		return (xfunc_func_expense((LispValue) get_function(clause),
								   (LispValue) get_funcargs(clause)));
	else if (fast_not_clause(clause))
		return (xfunc_local_expense(lsecond(clause)));
	else if (fast_or_clause(clause) || fast_and_clause(clause))
	{
		/* find cost of evaluating each disjunct */
		for (tmpclause = lnext(clause); tmpclause != LispNil;
			 tmpclause = lnext(tmpclause))
			cost += xfunc_local_expense(lfirst(tmpclause));
		return (cost);
	}
	else
	{
		elog(ERROR, "Clause node of undetermined type");
		return (-1);
	}
}

/*
 ** xfunc_func_expense --
 **    given a Func or Oper and its args, find its expense.
 ** Note: in Stonebraker's SIGMOD '91 paper, he uses a more complicated metric
 ** than the one here.	We can ignore the expected number of tuples for
 ** our calculations; we just need the per-tuple expense.  But he also
 ** proposes components to take into account the costs of accessing disk and
 ** archive.  We didn't adopt that scheme here; eventually the vacuum
 ** cleaner should be able to tell us what percentage of bytes to find on
 ** which storage level, and that should be multiplied in appropriately
 ** in the cost function below.  Right now we don't model the cost of
 ** accessing secondary or tertiary storage, since we don't have sufficient
 ** stats to do it right.
 */
Cost
xfunc_func_expense(LispValue node, LispValue args)
{
	HeapTuple	tupl;			/* the pg_proc tuple for each function */
	Form_pg_proc proc;			/* a data structure to hold the pg_proc
								 * tuple */
	int			width = 0;		/* byte width of the field referenced by
								 * each clause */
	RegProcedure funcid;		/* ID of function associate with node */
	Cost		cost = 0;		/* running expense */
	LispValue	tmpclause;
	LispValue	operand;		/* one operand of an operator */

	if (IsA(node, Oper))
	{
		/* don't trust the opid in the Oper node.  Use the opno. */
		if (!(funcid = get_opcode(get_opno((Oper) node))))
			elog(ERROR, "Oper's function is undefined");
	}
	else
		funcid = get_funcid((Func) node);

	/* look up tuple in cache */
	tupl = SearchSysCacheTuple(PROOID,
								ObjectIdGetDatum(funcid),
								0, 0, 0);
	if (!HeapTupleIsValid(tupl))
		elog(ERROR, "Cache lookup failed for procedure %d", funcid);
	proc = (Form_pg_proc) GETSTRUCT(tupl);

	/*
	 * * if it's a Postquel function, its cost is stored in the *
	 * associated plan.
	 */
	if (proc->prolang == SQLlanguageId)
	{
		LispValue	tmpplan;
		List		planlist;

		if (IsA(node, Oper) ||get_func_planlist((Func) node) == LispNil)
		{
			Oid		   *argOidVect;		/* vector of argtypes */
			char	   *pq_src; /* text of PQ function */
			int			nargs;	/* num args to PQ function */
			QueryTreeList *queryTree_list;		/* dummy variable */

			/*
			 * * plan the function, storing it in the Func node for later *
			 * use by the executor.
			 */
			pq_src = (char *) textout(&(proc->prosrc));
			nargs = proc->pronargs;
			if (nargs > 0)
				argOidVect = proc->proargtypes;
			planlist = (List) pg_parse_and_plan(pq_src, argOidVect, nargs,
												&parseTree_list, None, FALSE);
			if (IsA(node, Func))
				set_func_planlist((Func) node, planlist);

		}
		else
		{						/* plan has been cached inside the Func
								 * node already */
			planlist = get_func_planlist((Func) node);
		}

		/*
		 * * Return the sum of the costs of the plans (the PQ function *
		 * may have many queries in its body).
		 */
		foreach(tmpplan, planlist)
			cost += get_cost((Plan) lfirst(tmpplan));
		return (cost);
	}
	else
	{							/* it's a C function */

		/*
		 * *  find the cost of evaluating the function's arguments *  and
		 * the width of the operands
		 */
		for (tmpclause = args; tmpclause != LispNil;
			 tmpclause = lnext(tmpclause))
		{

			if ((operand = lfirst(tmpclause)) != LispNil)
			{
				cost += xfunc_local_expense(operand);
				width += xfunc_width(operand);
			}
		}

		/*
		 * * when stats become available, add in cost of accessing
		 * secondary * and tertiary storage here.
		 */
		return (cost +
				(Cost) proc->propercall_cpu +
		(Cost) proc->properbyte_cpu * (Cost) proc->probyte_pct / 100.00 *
				(Cost) width

		/*
		 * Pct_of_obj_in_mem DISK_COST * proc->probyte_pct/100.00 * width
		 * Pct_of_obj_on_disk + ARCH_COST * proc->probyte_pct/100.00 *
		 * width Pct_of_obj_on_arch
		 */
			);
	}
}

/*
 ** xfunc_width --
 **    recursively find the width of a expression
 */

int
xfunc_width(LispValue clause)
{
	Relation	rd;				/* Relation Descriptor */
	HeapTuple	tupl;			/* structure to hold a cached tuple */
	TypeTupleForm type;			/* structure to hold a type tuple */
	int			retval = 0;

	if (IsA(clause, Const))
	{
		/* base case: width is the width of this constant */
		retval = get_constlen((Const) clause);
		goto exit;
	}
	else if (IsA(clause, ArrayRef))
	{
		/* base case: width is width of the refelem within the array */
		retval = get_refelemlength((ArrayRef) clause);
		goto exit;
	}
	else if (IsA(clause, Var))
	{
		/* base case: width is width of this attribute */
		tupl = SearchSysCacheTuple(TYPOID,
								  ObjectIdGetDatum(get_vartype((Var) clause)),
								   0, 0, 0);
		if (!HeapTupleIsValid(tupl))
			elog(ERROR, "Cache lookup failed for type %d",
				 get_vartype((Var) clause));
		type = (TypeTupleForm) GETSTRUCT(tupl);
		if (get_varattno((Var) clause) == 0)
		{
			/* clause is a tuple.  Get its width */
			rd = heap_open(type->typrelid);
			retval = xfunc_tuple_width(rd);
			heap_close(rd);
		}
		else
		{
			/* attribute is a base type */
			retval = type->typlen;
		}
		goto exit;
	}
	else if (IsA(clause, Param))
	{
		if (typeidTypeRelid(get_paramtype((Param) clause)))
		{
			/* Param node returns a tuple.	Find its width */
			rd = heap_open(typeidTypeRelid(get_paramtype((Param) clause)));
			retval = xfunc_tuple_width(rd);
			heap_close(rd);
		}
		else if (get_param_tlist((Param) clause) != LispNil)
		{
			/* Param node projects a complex type */
			Assert(length(get_param_tlist((Param) clause)) == 1);		/* sanity */
			retval =
				xfunc_width((LispValue)
					  get_expr(lfirst(get_param_tlist((Param) clause))));
		}
		else
		{
			/* Param node returns a base type */
			retval = typeLen(typeidType(get_paramtype((Param) clause)));
		}
		goto exit;
	}
	else if (IsA(clause, Iter))
	{

		/*
		 * * An Iter returns a setof things, so return the width of a
		 * single * thing. * Note:	THIS MAY NOT WORK RIGHT WHEN AGGS GET
		 * FIXED, * SINCE AGG FUNCTIONS CHEW ON THE WHOLE SETOF THINGS!!!! *
		 * This whole Iter business is bogus, anyway.
		 */
		retval = xfunc_width(get_iterexpr((Iter) clause));
		goto exit;
	}
	else if (fast_is_clause(clause))
	{

		/*
		 * * get function associated with this Oper, and treat this as * a
		 * Func
		 */
		tupl = SearchSysCacheTuple(OPROID,
					   ObjectIdGetDatum(get_opno((Oper) get_op(clause))),
								   0, 0, 0);
		if (!HeapTupleIsValid(tupl))
			elog(ERROR, "Cache lookup failed for procedure %d",
				 get_opno((Oper) get_op(clause)));
		return (xfunc_func_width
				((RegProcedure) (((OperatorTupleForm) (GETSTRUCT(tupl)))->oprcode),
				 (LispValue) get_opargs(clause)));
	}
	else if (fast_is_funcclause(clause))
	{
		Func		func = (Func) get_function(clause);

		if (get_func_tlist(func) != LispNil)
		{

			/*
			 * this function has a projection on it.  Get the length of
			 * the projected attribute
			 */
			Assert(length(get_func_tlist(func)) == 1);	/* sanity */
			retval =
				xfunc_width((LispValue)
							get_expr(lfirst(get_func_tlist(func))));
			goto exit;
		}
		else
		{
			return (xfunc_func_width((RegProcedure) get_funcid(func),
									 (LispValue) get_funcargs(clause)));
		}
	}
	else
	{
		elog(ERROR, "Clause node of undetermined type");
		return (-1);
	}

exit:
	if (retval == -1)
		retval = VARLEN_DEFAULT;
	return (retval);
}

/*
 ** xfunc_card_unreferenced:
 **   find all relations not referenced in clause, and multiply their
 ** cardinalities.	Ignore relation of cardinality 0.
 ** User may pass in referenced list, if they know it (useful
 ** for joins).
 */
static Count
xfunc_card_unreferenced(Query *queryInfo,
						LispValue clause, Relid referenced)
{
	Relid		unreferenced,
				allrelids = LispNil;
	LispValue	temp;

	/* find all relids of base relations referenced in query */
	foreach(temp, queryInfo->base_rel_list)
	{
		Assert(lnext(get_relids((RelOptInfo) lfirst(temp))) == LispNil);
		allrelids = lappend(allrelids,
							lfirst(get_relids((RelOptInfo) lfirst(temp))));
	}

	/* find all relids referenced in query but not in clause */
	if (!referenced)
		referenced = xfunc_find_references(clause);
	unreferenced = set_difference(allrelids, referenced);

	return (xfunc_card_product(unreferenced));
}

/*
 ** xfunc_card_product
 **   multiple together cardinalities of a list relations.
 */
Count
xfunc_card_product(Query *queryInfo, Relid relids)
{
	LispValue	cinfonode;
	LispValue	temp;
	RelOptInfo			currel;
	Cost		tuples;
	Count		retval = 0;

	foreach(temp, relids)
	{
		currel = get_rel(lfirst(temp));
		tuples = get_tuples(currel);

		if (tuples)
		{						/* not of cardinality 0 */
			/* factor in the selectivity of all zero-cost clauses */
			foreach(cinfonode, get_clauseinfo(currel))
			{
				if (!xfunc_expense(queryInfo, get_clause((CInfo) lfirst(cinfonode))))
					tuples *=
						compute_clause_selec(queryInfo,
								   get_clause((CInfo) lfirst(cinfonode)),
											 LispNil);
			}

			if (retval == 0)
				retval = tuples;
			else
				retval *= tuples;
		}
	}
	if (retval == 0)
		retval = 1;				/* saves caller from dividing by zero */
	return (retval);
}


/*
 ** xfunc_find_references:
 **   Traverse a clause and find all relids referenced in the clause.
 */
List
xfunc_find_references(LispValue clause)
{
	List		retval = (List) LispNil;
	LispValue	tmpclause;

	/* Base cases */
	if (IsA(clause, Var))
		return (lispCons(lfirst(get_varid((Var) clause)), LispNil));
	else if (IsA(clause, Const) ||IsA(clause, Param))
		return ((List) LispNil);

	/* recursion */
	else if (IsA(clause, Iter))

		/*
		 * Too low. Should multiply by the expected number of iterations.
		 * maybe
		 */
		return (xfunc_find_references(get_iterexpr((Iter) clause)));
	else if (IsA(clause, ArrayRef))
		return (xfunc_find_references(get_refexpr((ArrayRef) clause)));
	else if (fast_is_clause(clause))
	{
		/* string together result of all operands of Oper */
		for (tmpclause = (LispValue) get_opargs(clause); tmpclause != LispNil;
			 tmpclause = lnext(tmpclause))
			retval = nconc(retval, xfunc_find_references(lfirst(tmpclause)));
		return (retval);
	}
	else if (fast_is_funcclause(clause))
	{
		/* string together result of all args of Func */
		for (tmpclause = (LispValue) get_funcargs(clause);
			 tmpclause != LispNil;
			 tmpclause = lnext(tmpclause))
			retval = nconc(retval, xfunc_find_references(lfirst(tmpclause)));
		return (retval);
	}
	else if (fast_not_clause(clause))
		return (xfunc_find_references(lsecond(clause)));
	else if (fast_or_clause(clause) || fast_and_clause(clause))
	{
		/* string together result of all operands of OR */
		for (tmpclause = lnext(clause); tmpclause != LispNil;
			 tmpclause = lnext(tmpclause))
			retval = nconc(retval, xfunc_find_references(lfirst(tmpclause)));
		return (retval);
	}
	else
	{
		elog(ERROR, "Clause node of undetermined type");
		return ((List) LispNil);
	}
}

/*
 ** xfunc_primary_join:
 **   Find the primary join clause: for Hash and Merge Joins, this is the
 ** min rank Hash or Merge clause, while for Nested Loop it's the
 ** min rank pathclause
 */
LispValue
xfunc_primary_join(JoinPath pathnode)
{
	LispValue	joinclauselist = get_pathclauseinfo(pathnode);
	CInfo		mincinfo;
	LispValue	tmplist;
	LispValue	minclause = LispNil;
	Cost		minrank,
				tmprank;

	if (IsA(pathnode, MergePath))
	{
		for (tmplist = get_path_mergeclauses((MergePath) pathnode),
			 minclause = lfirst(tmplist),
			 minrank = xfunc_rank(minclause);
			 tmplist != LispNil;
			 tmplist = lnext(tmplist))
			if ((tmprank = xfunc_rank(lfirst(tmplist)))
				< minrank)
			{
				minrank = tmprank;
				minclause = lfirst(tmplist);
			}
		return (minclause);
	}
	else if (IsA(pathnode, HashPath))
	{
		for (tmplist = get_path_hashclauses((HashPath) pathnode),
			 minclause = lfirst(tmplist),
			 minrank = xfunc_rank(minclause);
			 tmplist != LispNil;
			 tmplist = lnext(tmplist))
			if ((tmprank = xfunc_rank(lfirst(tmplist)))
				< minrank)
			{
				minrank = tmprank;
				minclause = lfirst(tmplist);
			}
		return (minclause);
	}

	/* if we drop through, it's nested loop join */
	if (joinclauselist == LispNil)
		return (LispNil);

	for (tmplist = joinclauselist, mincinfo = (CInfo) lfirst(joinclauselist),
		 minrank = xfunc_rank(get_clause((CInfo) lfirst(tmplist)));
		 tmplist != LispNil;
		 tmplist = lnext(tmplist))
		if ((tmprank = xfunc_rank(get_clause((CInfo) lfirst(tmplist))))
			< minrank)
		{
			minrank = tmprank;
			mincinfo = (CInfo) lfirst(tmplist);
		}
	return ((LispValue) get_clause(mincinfo));
}

/*
 ** xfunc_get_path_cost
 **   get the expensive function costs of the path
 */
Cost
xfunc_get_path_cost(Query *queryInfo, Path pathnode)
{
	Cost		cost = 0;
	LispValue	tmplist;
	Cost		selec = 1.0;

	/*
	 * * first add in the expensive local function costs. * We ensure that
	 * the clauses are sorted by rank, so that we * know (via
	 * selectivities) the number of tuples that will be checked * by each
	 * function.  If we're not doing any optimization of expensive *
	 * functions, we don't sort.
	 */
	if (XfuncMode != XFUNC_OFF)
		set_locclauseinfo(pathnode, lisp_qsort(get_locclauseinfo(pathnode),
											   xfunc_cinfo_compare));
	for (tmplist = get_locclauseinfo(pathnode), selec = 1.0;
		 tmplist != LispNil;
		 tmplist = lnext(tmplist))
	{
		cost += (Cost) (xfunc_local_expense(get_clause((CInfo) lfirst(tmplist)))
					  * (Cost) get_tuples(get_parent(pathnode)) * selec);
		selec *= compute_clause_selec(queryInfo,
									  get_clause((CInfo) lfirst(tmplist)),
									  LispNil);
	}

	/*
	 * * Now add in any node-specific expensive function costs. * Again,
	 * we must ensure that the clauses are sorted by rank.
	 */
	if (IsA(pathnode, JoinPath))
	{
		if (XfuncMode != XFUNC_OFF)
			set_pathclauseinfo((JoinPath) pathnode, lisp_qsort
							   (get_pathclauseinfo((JoinPath) pathnode),
								xfunc_cinfo_compare));
		for (tmplist = get_pathclauseinfo((JoinPath) pathnode), selec = 1.0;
			 tmplist != LispNil;
			 tmplist = lnext(tmplist))
		{
			cost += (Cost) (xfunc_local_expense(get_clause((CInfo) lfirst(tmplist)))
					  * (Cost) get_tuples(get_parent(pathnode)) * selec);
			selec *= compute_clause_selec(queryInfo,
									 get_clause((CInfo) lfirst(tmplist)),
										  LispNil);
		}
	}
	if (IsA(pathnode, HashPath))
	{
		if (XfuncMode != XFUNC_OFF)
			set_path_hashclauses
				((HashPath) pathnode,
				 lisp_qsort(get_path_hashclauses((HashPath) pathnode),
							xfunc_clause_compare));
		for (tmplist = get_path_hashclauses((HashPath) pathnode), selec = 1.0;
			 tmplist != LispNil;
			 tmplist = lnext(tmplist))
		{
			cost += (Cost) (xfunc_local_expense(lfirst(tmplist))
					  * (Cost) get_tuples(get_parent(pathnode)) * selec);
			selec *= compute_clause_selec(queryInfo,
										  lfirst(tmplist), LispNil);
		}
	}
	if (IsA(pathnode, MergePath))
	{
		if (XfuncMode != XFUNC_OFF)
			set_path_mergeclauses
				((MergePath) pathnode,
				 lisp_qsort(get_path_mergeclauses((MergePath) pathnode),
							xfunc_clause_compare));
		for (tmplist = get_path_mergeclauses((MergePath) pathnode), selec = 1.0;
			 tmplist != LispNil;
			 tmplist = lnext(tmplist))
		{
			cost += (Cost) (xfunc_local_expense(lfirst(tmplist))
					  * (Cost) get_tuples(get_parent(pathnode)) * selec);
			selec *= compute_clause_selec(queryInfo,
										  lfirst(tmplist), LispNil);
		}
	}
	Assert(cost >= 0);
	return (cost);
}

/*
 ** Recalculate the cost of a path node.  This includes the basic cost of the
 ** node, as well as the cost of its expensive functions.
 ** We need to do this to the parent after pulling a clause from a child into a
 ** parent.  Thus we should only be calling this function on JoinPaths.
 */
Cost
xfunc_total_path_cost(JoinPath pathnode)
{
	Cost		cost = xfunc_get_path_cost((Path) pathnode);

	Assert(IsA(pathnode, JoinPath));
	if (IsA(pathnode, MergePath))
	{
		MergePath	mrgnode = (MergePath) pathnode;

		cost += cost_mergejoin(get_path_cost((Path) get_outerjoinpath(mrgnode)),
						get_path_cost((Path) get_innerjoinpath(mrgnode)),
							   get_outersortkeys(mrgnode),
							   get_innersortkeys(mrgnode),
						   get_tuples(get_parent((Path) get_outerjoinpath
												 (mrgnode))),
						   get_tuples(get_parent((Path) get_innerjoinpath
												 (mrgnode))),
							get_width(get_parent((Path) get_outerjoinpath
												 (mrgnode))),
							get_width(get_parent((Path) get_innerjoinpath
												 (mrgnode))));
		Assert(cost >= 0);
		return (cost);
	}
	else if (IsA(pathnode, HashPath))
	{
		HashPath hashnode = (HashPath) pathnode;

		cost += cost_hashjoin(get_path_cost((Path) get_outerjoinpath(hashnode)),
					   get_path_cost((Path) get_innerjoinpath(hashnode)),
							  get_outerhashkeys(hashnode),
							  get_innerhashkeys(hashnode),
						   get_tuples(get_parent((Path) get_outerjoinpath
												 (hashnode))),
						   get_tuples(get_parent((Path) get_innerjoinpath
												 (hashnode))),
							get_width(get_parent((Path) get_outerjoinpath
												 (hashnode))),
							get_width(get_parent((Path) get_innerjoinpath
												 (hashnode))));
		Assert(cost >= 0);
		return (cost);
	}
	else
/* Nested Loop Join */
	{
		cost += cost_nestloop(get_path_cost((Path) get_outerjoinpath(pathnode)),
					   get_path_cost((Path) get_innerjoinpath(pathnode)),
						   get_tuples(get_parent((Path) get_outerjoinpath
												 (pathnode))),
						   get_tuples(get_parent((Path) get_innerjoinpath
												 (pathnode))),
							get_pages(get_parent((Path) get_outerjoinpath
												 (pathnode))),
							IsA(get_innerjoinpath(pathnode), IndexPath));
		Assert(cost >= 0);
		return (cost);
	}
}


/*
 ** xfunc_expense_per_tuple --
 **    return the expense of the join *per-tuple* of the input relation.
 ** The cost model here is that a join costs
 **		k*card(outer)*card(inner) + l*card(outer) + m*card(inner) + n
 **
 ** We treat the l and m terms by considering them to be like restrictions
 ** constrained to be right under the join.  Thus the cost per inner and
 ** cost per outer of the join is different, reflecting these virtual nodes.
 **
 ** The cost per tuple of outer is k + l/referenced(inner).  Cost per tuple
 ** of inner is k + m/referenced(outer).
 ** The constants k, l, m and n depend on the join method.	Measures here are
 ** based on the costs in costsize.c, with fudging for HashJoin and Sorts to
 ** make it fit our model (the 'q' in HashJoin results in a
 ** card(outer)/card(inner) term, and sorting results in a log term.

 */
Cost
xfunc_expense_per_tuple(JoinPath joinnode, int whichchild)
{
	RelOptInfo			outerrel = get_parent((Path) get_outerjoinpath(joinnode));
	RelOptInfo			innerrel = get_parent((Path) get_innerjoinpath(joinnode));
	Count		outerwidth = get_width(outerrel);
	Count		outers_per_page = ceil(BLCKSZ / (outerwidth + sizeof(HeapTupleData)));

	if (IsA(joinnode, HashPath))
	{
		if (whichchild == INNER)
			return ((1 + _CPU_PAGE_WEIGHT_) * outers_per_page / NBuffers);
		else
			return (((1 + _CPU_PAGE_WEIGHT_) * outers_per_page / NBuffers)
					+ _CPU_PAGE_WEIGHT_
					/ xfunc_card_product(get_relids(innerrel)));
	}
	else if (IsA(joinnode, MergePath))
	{
		/* assumes sort exists, and costs one (I/O + CPU) per tuple */
		if (whichchild == INNER)
			return ((2 * _CPU_PAGE_WEIGHT_ + 1)
					/ xfunc_card_product(get_relids(outerrel)));
		else
			return ((2 * _CPU_PAGE_WEIGHT_ + 1)
					/ xfunc_card_product(get_relids(innerrel)));
	}
	else
/* nestloop */
	{
		Assert(IsA(joinnode, JoinPath));
		return (_CPU_PAGE_WEIGHT_);
	}
}

/*
 ** xfunc_fixvars --
 ** After pulling up a clause, we must walk its expression tree, fixing Var
 ** nodes to point to the correct varno (either INNER or OUTER, depending
 ** on which child the clause was pulled from), and the right varattno in the
 ** target list of the child's former relation.  If the target list of the
 ** child RelOptInfo does not contain the attribute we need, we add it.
 */
void
xfunc_fixvars(LispValue clause, /* clause being pulled up */
			  RelOptInfo rel,			/* rel it's being pulled from */
			  int varno)		/* whether rel is INNER or OUTER of join */
{
	LispValue	tmpclause;		/* temporary variable */
	TargetEntry *tle;			/* tlist member corresponding to var */


	if (IsA(clause, Const) ||IsA(clause, Param))
		return;
	else if (IsA(clause, Var))
	{
		/* here's the meat */
		tle = tlistentry_member((Var) clause, get_targetlist(rel));
		if (tle == LispNil)
		{

			/*
			 * * The attribute we need is not in the target list, * so we
			 * have to add it. *
			 *
			 */
			add_tl_element(rel, (Var) clause);
			tle = tlistentry_member((Var) clause, get_targetlist(rel));
		}
		set_varno(((Var) clause), varno);
		set_varattno(((Var) clause), get_resno(get_resdom(get_entry(tle))));
	}
	else if (IsA(clause, Iter))
		xfunc_fixvars(get_iterexpr((Iter) clause), rel, varno);
	else if (fast_is_clause(clause))
	{
		xfunc_fixvars(lfirst(lnext(clause)), rel, varno);
		xfunc_fixvars(lfirst(lnext(lnext(clause))), rel, varno);
	}
	else if (fast_is_funcclause(clause))
		for (tmpclause = lnext(clause); tmpclause != LispNil;
			 tmpclause = lnext(tmpclause))
			xfunc_fixvars(lfirst(tmpclause), rel, varno);
	else if (fast_not_clause(clause))
		xfunc_fixvars(lsecond(clause), rel, varno);
	else if (fast_or_clause(clause) || fast_and_clause(clause))
		for (tmpclause = lnext(clause); tmpclause != LispNil;
			 tmpclause = lnext(tmpclause))
			xfunc_fixvars(lfirst(tmpclause), rel, varno);
	else
		elog(ERROR, "Clause node of undetermined type");
}


/*
 ** Comparison function for lisp_qsort() on a list of CInfo's.
 ** arg1 and arg2 should really be of type (CInfo *).
 */
int
xfunc_cinfo_compare(void *arg1, void *arg2)
{
	CInfo		info1 = *(CInfo *) arg1;
	CInfo		info2 = *(CInfo *) arg2;

	LispValue	clause1 = (LispValue) get_clause(info1),
				clause2 = (LispValue) get_clause(info2);

	return (xfunc_clause_compare((void *) &clause1, (void *) &clause2));
}

/*
 ** xfunc_clause_compare: comparison function for lisp_qsort() that compares two
 ** clauses based on expense/(1 - selectivity)
 ** arg1 and arg2 are really pointers to clauses.
 */
int
xfunc_clause_compare(void *arg1, void *arg2)
{
	LispValue	clause1 = *(LispValue *) arg1;
	LispValue	clause2 = *(LispValue *) arg2;
	Cost		rank1,			/* total xfunc rank of clause1 */
				rank2;			/* total xfunc rank of clause2 */

	rank1 = xfunc_rank(clause1);
	rank2 = xfunc_rank(clause2);

	if (rank1 < rank2)
		return (-1);
	else if (rank1 == rank2)
		return (0);
	else
		return (1);
}

/*
 ** xfunc_disjunct_sort --
 **   given a list of clauses, for each clause sort the disjuncts by cost
 **   (this assumes the predicates have been converted to Conjunctive NF)
 **   Modifies the clause list!
 */
void
xfunc_disjunct_sort(LispValue clause_list)
{
	LispValue	temp;

	foreach(temp, clause_list)
		if (or_clause(lfirst(temp)))
		lnext(lfirst(temp)) =
			lisp_qsort(lnext(lfirst(temp)), xfunc_disjunct_compare);
}


/*
 ** xfunc_disjunct_compare: comparison function for qsort() that compares two
 ** disjuncts based on cost/selec.
 ** arg1 and arg2 are really pointers to disjuncts
 */
int
xfunc_disjunct_compare(Query *queryInfo, void *arg1, void *arg2)
{
	LispValue	disjunct1 = *(LispValue *) arg1;
	LispValue	disjunct2 = *(LispValue *) arg2;
	Cost		cost1,			/* total cost of disjunct1 */
				cost2,			/* total cost of disjunct2 */
				selec1,
				selec2;
	Cost		rank1,
				rank2;

	cost1 = xfunc_expense(queryInfo, disjunct1);
	cost2 = xfunc_expense(queryInfo, disjunct2);
	selec1 = compute_clause_selec(queryInfo,
								  disjunct1, LispNil);
	selec2 = compute_clause_selec(queryInfo,
								  disjunct2, LispNil);

	if (selec1 == 0)
		rank1 = MAXFLOAT;
	else if (cost1 == 0)
		rank1 = 0;
	else
		rank1 = cost1 / selec1;

	if (selec2 == 0)
		rank2 = MAXFLOAT;
	else if (cost2 == 0)
		rank2 = 0;
	else
		rank2 = cost2 / selec2;

	if (rank1 < rank2)
		return (-1);
	else if (rank1 == rank2)
		return (0);
	else
		return (1);
}

/* ------------------------ UTILITY FUNCTIONS ------------------------------- */
/*
 ** xfunc_func_width --
 **    Given a function OID and operands, find the width of the return value.
 */
int
xfunc_func_width(RegProcedure funcid, LispValue args)
{
	Relation	rd;				/* Relation Descriptor */
	HeapTuple	tupl;			/* structure to hold a cached tuple */
	Form_pg_proc proc;			/* structure to hold the pg_proc tuple */
	TypeTupleForm type;			/* structure to hold the pg_type tuple */
	LispValue	tmpclause;
	int			retval;

	/* lookup function and find its return type */
	Assert(RegProcedureIsValid(funcid));
	tupl = SearchSysCacheTuple(PROOID,
								ObjectIdGetDatum(funcid),
								0, 0, 0);
	if (!HeapTupleIsValid(tupl))
		elog(ERROR, "Cache lookup failed for procedure %d", funcid);
	proc = (Form_pg_proc) GETSTRUCT(tupl);

	/* if function returns a tuple, get the width of that */
	if (typeidTypeRelid(proc->prorettype))
	{
		rd = heap_open(typeidTypeRelid(proc->prorettype));
		retval = xfunc_tuple_width(rd);
		heap_close(rd);
		goto exit;
	}
	else
/* function returns a base type */
	{
		tupl = SearchSysCacheTuple(TYPOID,
								   ObjectIdGetDatum(proc->prorettype),
								   0, 0, 0);
		if (!HeapTupleIsValid(tupl))
			elog(ERROR, "Cache lookup failed for type %d", proc->prorettype);
		type = (TypeTupleForm) GETSTRUCT(tupl);
		/* if the type length is known, return that */
		if (type->typlen != -1)
		{
			retval = type->typlen;
			goto exit;
		}
		else
/* estimate the return size */
		{
			/* find width of the function's arguments */
			for (tmpclause = args; tmpclause != LispNil;
				 tmpclause = lnext(tmpclause))
				retval += xfunc_width(lfirst(tmpclause));
			/* multiply by outin_ratio */
			retval = (int) (proc->prooutin_ratio / 100.0 * retval);
			goto exit;
		}
	}
exit:
	return (retval);
}

/*
 ** xfunc_tuple_width --
 **		Return the sum of the lengths of all the attributes of a given relation
 */
int
xfunc_tuple_width(Relation rd)
{
	int			i;
	int			retval = 0;
	TupleDesc	tdesc = RelationGetTupleDescriptor(rd);

	for (i = 0; i < tdesc->natts; i++)
	{
		if (tdesc->attrs[i]->attlen != -1)
			retval += tdesc->attrs[i]->attlen;
		else
			retval += VARLEN_DEFAULT;
	}

	return (retval);
}

/*
 ** xfunc_num_join_clauses --
 **   Find the number of join clauses associated with this join path
 */
int
xfunc_num_join_clauses(JoinPath path)
{
	int			num = length(get_pathclauseinfo(path));

	if (IsA(path, MergePath))
		return (num + length(get_path_mergeclauses((MergePath) path)));
	else if (IsA(path, HashPath))
		return (num + length(get_path_hashclauses((HashPath) path)));
	else
		return (num);
}

/*
 ** xfunc_LispRemove --
 **   Just like LispRemove, but it whines if the item to be removed ain't there
 */
LispValue
xfunc_LispRemove(LispValue foo, List bar)
{
	LispValue	temp = LispNil;
	LispValue	result = LispNil;
	int			sanity = false;

	for (temp = bar; !null(temp); temp = lnext(temp))
		if (!equal((Node) (foo), (Node) (lfirst(temp))))
			result = lappend(result, lfirst(temp));
		else
			sanity = true;		/* found a matching item to remove! */

	if (!sanity)
		elog(ERROR, "xfunc_LispRemove: didn't find a match!");

	return (result);
}

#define Node_Copy(a, b, c, d) \
do { \
	if (NodeCopy((Node)((a)->d), (Node*)&((b)->d), c) != true) \
	{ \
		return false; \
	} \
} while(0)

/*
 ** xfunc_copyrel --
 **   Just like _copyRel, but doesn't copy the paths
 */
bool
xfunc_copyrel(RelOptInfo from, RelOptInfo *to)
{
	RelOptInfo			newnode;

	Pointer		(*alloc) () = palloc;

	/* COPY_CHECKARGS() */
	if (to == NULL)
		return false;

	/* COPY_CHECKNULL() */
	if (from == NULL)
	{
		(*to) = NULL;
		return true;
	}

	/* COPY_NEW(c) */
	newnode = (RelOptInfo) (*alloc) (classSize(RelOptInfo));
	if (newnode == NULL)
		return false;

	/* ----------------
	 *	copy node superclass fields
	 * ----------------
	 */
	CopyNodeFields((Node) from, (Node) newnode, alloc);

	/* ----------------
	 *	copy remainder of node
	 * ----------------
	 */
	Node_Copy(from, newnode, alloc, relids);

	newnode->indexed = from->indexed;
	newnode->pages = from->pages;
	newnode->tuples = from->tuples;
	newnode->size = from->size;
	newnode->width = from->width;

	Node_Copy(from, newnode, alloc, targetlist);

	/*
	 * No!!!!	 Node_Copy(from, newnode, alloc, pathlist);
	 * Node_Copy(from, newnode, alloc, unorderedpath); Node_Copy(from,
	 * newnode, alloc, cheapestpath);
	 */
#if 0							/* can't use Node_copy now. 2/95 -ay */
	Node_Copy(from, newnode, alloc, classlist);
	Node_Copy(from, newnode, alloc, indexkeys);
	Node_Copy(from, newnode, alloc, ordering);
#endif
	Node_Copy(from, newnode, alloc, clauseinfo);
	Node_Copy(from, newnode, alloc, joininfo);
	Node_Copy(from, newnode, alloc, innerjoin);
	Node_Copy(from, newnode, alloc, superrels);

	(*to) = newnode;
	return true;
}
