/*-------------------------------------------------------------------------
 *
 * parse_clause.c--
 *	  handle clauses in parser
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_clause.c,v 1.7 1998/01/05 03:32:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "postgres.h"
#include "access/heapam.h"
#include "catalog/pg_type.h"
#include "parser/analyze.h"
#include "parser/parse_clause.h"
#include "parser/parse_expr.h"
#include "parser/parse_node.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"

static TargetEntry *find_targetlist_entry(ParseState *pstate,
			SortGroupBy *sortgroupby, List *tlist);
static void parseFromClause(ParseState *pstate, List *frmList);

/*
 * makeRangeTable -
 *	  make a range table with the specified relation (optional) and the
 *	  from-clause.
 */
void
makeRangeTable(ParseState *pstate, char *relname, List *frmList)
{
	RangeTblEntry *rte;

	parseFromClause(pstate, frmList);

	if (relname == NULL)
		return;

	if (refnameRangeTablePosn(pstate->p_rtable, relname) < 1)
		rte = addRangeTableEntry(pstate, relname, relname, FALSE, FALSE);
	else
		rte = refnameRangeTableEntry(pstate->p_rtable, relname);

	pstate->p_target_rangetblentry = rte;
	Assert(pstate->p_target_relation == NULL);
	pstate->p_target_relation = heap_open(rte->relid);
	Assert(pstate->p_target_relation != NULL);
	/* will close relation later */
}

/*
 * transformWhereClause -
 *	  transforms the qualification and make sure it is of type Boolean
 *
 */
Node *
transformWhereClause(ParseState *pstate, Node *a_expr)
{
	Node	   *qual;

	if (a_expr == NULL)
		return (Node *) NULL;	/* no qualifiers */

	pstate->p_in_where_clause = true;
	qual = transformExpr(pstate, a_expr, EXPR_COLUMN_FIRST);
	pstate->p_in_where_clause = false;
	if (exprType(qual) != BOOLOID)
	{
		elog(ERROR,
			 "where clause must return type bool, not %s",
			 typeidTypeName(exprType(qual)));
	}
	return qual;
}

/*
 * parseFromClause -
 *	  turns the table references specified in the from-clause into a
 *	  range table. The range table may grow as we transform the expressions
 *	  in the target list. (Note that this happens because in POSTQUEL, we
 *	  allow references to relations not specified in the from-clause. We
 *	  also allow that in our POST-SQL)
 *
 */
static void
parseFromClause(ParseState *pstate, List *frmList)
{
	List	   *fl;

	foreach(fl, frmList)
	{
		RangeVar   *r = lfirst(fl);
		RelExpr    *baserel = r->relExpr;
		char	   *relname = baserel->relname;
		char	   *refname = r->name;
		RangeTblEntry *rte;

		if (refname == NULL)
			refname = relname;

		/*
		 * marks this entry to indicate it comes from the FROM clause. In
		 * SQL, the target list can only refer to range variables
		 * specified in the from clause but we follow the more powerful
		 * POSTQUEL semantics and automatically generate the range
		 * variable if not specified. However there are times we need to
		 * know whether the entries are legitimate.
		 *
		 * eg. select * from foo f where f.x = 1; will generate wrong answer
		 * if we expand * to foo.x.
		 */
		rte = addRangeTableEntry(pstate, relname, refname, baserel->inh, TRUE);
	}
}

/*
 *	find_targetlist_entry -
 *	  returns the Resdom in the target list matching the specified varname
 *	  and range
 *
 */
static TargetEntry *
find_targetlist_entry(ParseState *pstate, SortGroupBy *sortgroupby, List *tlist)
{
	List	   *i;
	int			real_rtable_pos = 0,
				target_pos = 0;
	TargetEntry *target_result = NULL;

	if (sortgroupby->range)
		real_rtable_pos = refnameRangeTablePosn(pstate->p_rtable,
												sortgroupby->range);

	foreach(i, tlist)
	{
		TargetEntry *target = (TargetEntry *) lfirst(i);
		Resdom	   *resnode = target->resdom;
		Var		   *var = (Var *) target->expr;
		char	   *resname = resnode->resname;
		int			test_rtable_pos = var->varno;

#ifdef PARSEDEBUG
		printf("find_targetlist_entry- target name is %s, position %d, resno %d\n",
			   (sortgroupby->name ? sortgroupby->name : "(null)"), target_pos + 1, sortgroupby->resno);
#endif

		if (!sortgroupby->name)
		{
			if (sortgroupby->resno == ++target_pos)
			{
				target_result = target;
				break;
			}
		}
		else
		{
			if (!strcmp(resname, sortgroupby->name))
			{
				if (sortgroupby->range)
				{
					if (real_rtable_pos == test_rtable_pos)
					{
						if (target_result != NULL)
							elog(ERROR, "Order/Group By '%s' is ambiguous", sortgroupby->name);
						else
							target_result = target;
					}
				}
				else
				{
					if (target_result != NULL)
						elog(ERROR, "Order/Group By '%s' is ambiguous", sortgroupby->name);
					else
						target_result = target;
				}
			}
		}
	}
	return target_result;
}

/*
 * transformGroupClause -
 *	  transform a Group By clause
 *
 */
List *
transformGroupClause(ParseState *pstate, List *grouplist, List *targetlist)
{
	List	   *glist = NIL,
			   *gl = NIL;

	while (grouplist != NIL)
	{
		GroupClause *grpcl = makeNode(GroupClause);
		TargetEntry *restarget;
		Resdom	   *resdom;

		restarget = find_targetlist_entry(pstate, lfirst(grouplist), targetlist);

		if (restarget == NULL)
			elog(ERROR, "The field being grouped by must appear in the target list");

		grpcl->entry = restarget;
		resdom = restarget->resdom;
		grpcl->grpOpoid = oprid(oper("<",
									 resdom->restype,
									 resdom->restype, false));
		if (glist == NIL)
			gl = glist = lcons(grpcl, NIL);
		else
		{
			List	   *i;
			
			foreach (i, glist)
			{
				GroupClause *gcl = (GroupClause *) lfirst (i);
				
				if ( gcl->entry == grpcl->entry )
					break;
			}
			if ( i == NIL )			/* not in grouplist already */
			{
				lnext(gl) = lcons(grpcl, NIL);
				gl = lnext(gl);
			}
			else
				pfree (grpcl);		/* get rid of this */
		}
		grouplist = lnext(grouplist);
	}

	return glist;
}

/*
 * transformSortClause -
 *	  transform an Order By clause
 *
 */
List *
transformSortClause(ParseState *pstate,
					List *orderlist,
					List *sortlist,
					List *targetlist,
					char *uniqueFlag)
{
	List	   *s = NIL;

	while (orderlist != NIL)
	{
		SortGroupBy *sortby = lfirst(orderlist);
		SortClause *sortcl = makeNode(SortClause);
		TargetEntry *restarget;
		Resdom	   *resdom;

		sortlist = NIL;	/* we create it on the fly here */
		
		restarget = find_targetlist_entry(pstate, sortby, targetlist);
		if (restarget == NULL)
			elog(ERROR, "The field being ordered by must appear in the target list");

		sortcl->resdom = resdom = restarget->resdom;
		sortcl->opoid = oprid(oper(sortby->useOp,
								   resdom->restype,
								   resdom->restype, false));
		if (sortlist == NIL)
		{
			s = sortlist = lcons(sortcl, NIL);
		}
		else
		{
			List	   *i;
			
			foreach (i, sortlist)
			{
				SortClause *scl = (SortClause *) lfirst (i);
				
				if ( scl->resdom == sortcl->resdom )
					break;
			}
			if ( i == NIL )			/* not in sortlist already */
			{
				lnext(s) = lcons(sortcl, NIL);
				s = lnext(s);
			}
			else
				pfree (sortcl);		/* get rid of this */
		}
		orderlist = lnext(orderlist);
	}

	if (uniqueFlag)
	{
		List	   *i;
		
		if (uniqueFlag[0] == '*')
		{

			/*
			 * concatenate all elements from target list that are not
			 * already in the sortby list
			 */
			foreach(i, targetlist)
			{
				TargetEntry *tlelt = (TargetEntry *) lfirst(i);

				s = sortlist;
				while (s != NIL)
				{
					SortClause *sortcl = lfirst(s);

					if (sortcl->resdom == tlelt->resdom)
						break;
					s = lnext(s);
				}
				if (s == NIL)
				{
					/* not a member of the sortclauses yet */
					SortClause *sortcl = makeNode(SortClause);

					sortcl->resdom = tlelt->resdom;
					sortcl->opoid = any_ordering_op(tlelt->resdom->restype);

					sortlist = lappend(sortlist, sortcl);
				}
			}
		}
		else
		{
			TargetEntry *tlelt = NULL;
			char	   *uniqueAttrName = uniqueFlag;

			/* only create sort clause with the specified unique attribute */
			foreach(i, targetlist)
			{
				tlelt = (TargetEntry *) lfirst(i);
				if (strcmp(tlelt->resdom->resname, uniqueAttrName) == 0)
					break;
			}
			if (i == NIL)
			{
				elog(ERROR, "The field specified in the UNIQUE ON clause is not in the targetlist");
			}
			s = sortlist;
			foreach(s, sortlist)
			{
				SortClause *sortcl = lfirst(s);

				if (sortcl->resdom == tlelt->resdom)
					break;
			}
			if (s == NIL)
			{
				/* not a member of the sortclauses yet */
				SortClause *sortcl = makeNode(SortClause);

				sortcl->resdom = tlelt->resdom;
				sortcl->opoid = any_ordering_op(tlelt->resdom->restype);

				sortlist = lappend(sortlist, sortcl);
			}
		}

	}

	return sortlist;
}

/*
 * transformUnionClause -
 *	  transform a Union clause
 *
 */
List *
transformUnionClause(List *unionClause, List *targetlist)
{
	List *union_list = NIL;
	QueryTreeList *qlist;
	int i;

	if (unionClause)
	{
		qlist = parse_analyze(unionClause);

		for (i=0; i < qlist->len; i++)
			union_list = lappend(union_list, qlist->qtrees[i]);
		/* we need to check return types are consistent here */
		return union_list;
	}
	else
		return NIL;
}
