/*-------------------------------------------------------------------------
 *
 * parse_clause.c--
 *	  handle clauses in parser
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_clause.c,v 1.18 1998/06/05 03:49:18 momjian Exp $
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
#include "parser/parse_coerce.h"


static TargetEntry *
find_targetlist_entry(ParseState *pstate,
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
	int			sublevels_up;

	parseFromClause(pstate, frmList);

	if (relname == NULL)
		return;

	if (refnameRangeTablePosn(pstate, relname, &sublevels_up) == 0 ||
		sublevels_up != 0)
		rte = addRangeTableEntry(pstate, relname, relname, FALSE, FALSE);
	else
		rte = refnameRangeTableEntry(pstate, relname);

	pstate->p_target_rangetblentry = rte;
	Assert(pstate->p_target_relation == NULL);
	pstate->p_target_relation = heap_open(rte->relid);
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
		return NULL;			/* no qualifiers */

	pstate->p_in_where_clause = true;
	qual = transformExpr(pstate, a_expr, EXPR_COLUMN_FIRST);
	pstate->p_in_where_clause = false;

	if (exprType(qual) != BOOLOID)
	{
		elog(ERROR, "WHERE clause must return type bool, not type %s",
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
 *	  also allow now as an extension.)
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
		real_rtable_pos = refnameRangeTablePosn(pstate, sortgroupby->range, NULL);

	foreach(i, tlist)
	{
		TargetEntry *target = (TargetEntry *) lfirst(i);
		Resdom	   *resnode = target->resdom;
		Var		   *var = (Var *) target->expr;
		char	   *resname = resnode->resname;
		int			test_rtable_pos = var->varno;

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
							elog(ERROR, "ORDER/GROUP BY '%s' is ambiguous", sortgroupby->name);
						else
							target_result = target;
					}
				}
				else
				{
					if (target_result != NULL)
						elog(ERROR, "ORDER/GROUP BY '%s' is ambiguous", sortgroupby->name);
					else
						target_result = target;
				}
			}
		}
	}

	/*    BEGIN add missing target entry hack.
	 *
	 *    Prior to this hack, this function returned NIL if no target_result.
	 *    Thus, ORDER/GROUP BY required the attributes be in the target list.
	 *    Now it constructs a new target entry which is appended to the end of
	 *    the target list.   This target is set to be  resjunk = TRUE so that
	 *    it will not be projected into the final tuple.
	 *          daveh@insightdist.com    5/20/98
	 */  
	if ( ! target_result && sortgroupby->name)   {
		List   *p_target = tlist;
		TargetEntry *tent = makeNode(TargetEntry);
		

		if (sortgroupby->range)  {
			Attr *missingTarget = (Attr *)makeNode(Attr);
			missingTarget->type = T_Attr;

			missingTarget->relname = palloc(strlen(sortgroupby->range) + 1);
			strcpy(missingTarget->relname, sortgroupby->range);

			missingTarget->attrs = lcons(makeString(sortgroupby->name), NIL);

			transformTargetId(pstate, (Node*)missingTarget, tent, sortgroupby->name, TRUE);
		}
		else  {
			Ident *missingTarget = (Ident *)makeNode(Ident);
			missingTarget->type = T_Ident;

			missingTarget->name = palloc(strlen(sortgroupby->name) + 1);
			strcpy(missingTarget->name, sortgroupby->name);

			transformTargetId(pstate, (Node*)missingTarget, tent, sortgroupby->name, TRUE);
		}


		/* Add to the end of the target list */
		while (lnext(p_target) != NIL)  {
			p_target = lnext(p_target);
		}
		lnext(p_target) = lcons(tent, NIL);
		target_result = tent;
	}
	/*    END add missing target entry hack.   */

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

			foreach(i, glist)
			{
				GroupClause *gcl = (GroupClause *) lfirst(i);

				if (gcl->entry == grpcl->entry)
					break;
			}
			if (i == NIL)		/* not in grouplist already */
			{
				lnext(gl) = lcons(grpcl, NIL);
				gl = lnext(gl);
			}
			else
				pfree(grpcl);	/* get rid of this */
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


		restarget = find_targetlist_entry(pstate, sortby, targetlist);
		sortcl->resdom = resdom = restarget->resdom;
		sortcl->opoid = oprid(oper(sortby->useOp,
								   resdom->restype,
								   resdom->restype, false));
		if (sortlist == NIL)
			s = sortlist = lcons(sortcl, NIL);
		else
		{
			List	   *i;

			foreach(i, sortlist)
			{
				SortClause *scl = (SortClause *) lfirst(i);

				if (scl->resdom == sortcl->resdom)
					break;
			}
			if (i == NIL)		/* not in sortlist already */
			{
				lnext(s) = lcons(sortcl, NIL);
				s = lnext(s);
			}
			else
				pfree(sortcl);	/* get rid of this */
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

					/*
					 *	We use equal() here because we are called for UNION
					 *	from the optimizer, and at that point, the sort clause
					 *	resdom pointers don't match the target list resdom
					 *	pointers
					 */
					if (equal(sortcl->resdom, tlelt->resdom))
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
				elog(ERROR, "All fields in the UNIQUE ON clause must appear in the target list");

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

/* transformUnionClause()
 * Transform a UNION clause.
 * Note that the union clause is actually a fully-formed select structure.
 * So, it is evaluated as a select, then the resulting target fields
 *  are matched up to ensure correct types in the results.
 * The select clause parsing is done recursively, so the unions are evaluated
 *  right-to-left. One might want to look at all columns from all clauses before
 *  trying to coerce, but unless we keep track of the call depth we won't know
 *  when to do this because of the recursion.
 * Let's just try matching in pairs for now (right to left) and see if it works.
 * - thomas 1998-05-22
 */
List *
transformUnionClause(List *unionClause, List *targetlist)
{
	List		  *union_list = NIL;
	QueryTreeList *qlist;
	int			   i;

	if (unionClause)
	{
		/* recursion */
		qlist = parse_analyze(unionClause, NULL);

		for (i = 0; i < qlist->len; i++)
		{
			List	   *prev_target = targetlist;
			List	   *next_target;
			
			if (length(targetlist) != length(qlist->qtrees[i]->targetList))
				elog(ERROR,"Each UNION clause must have the same number of columns");
				
			foreach(next_target, qlist->qtrees[i]->targetList)
			{
				Oid   itype;
				Oid   otype;
				otype = ((TargetEntry *)lfirst(prev_target))->resdom->restype;
				itype = ((TargetEntry *)lfirst(next_target))->resdom->restype;
				if (itype != otype)
				{
					Node *expr;

					expr = ((TargetEntry *)lfirst(next_target))->expr;
					expr = coerce_target_expr(NULL, expr, itype, otype);
					if (expr == NULL)
					{
						elog(ERROR,"Unable to transform %s to %s"
							 "\n\tEach UNION clause must have compatible target types",
							 typeidTypeName(itype),
							 typeidTypeName(otype));
					}
					((TargetEntry *)lfirst(next_target))->expr = expr;
					((TargetEntry *)lfirst(next_target))->resdom->restype = otype;
				}
				/* both are UNKNOWN? then evaluate as text... */
				else if (itype == UNKNOWNOID)
				{
					((TargetEntry *)lfirst(next_target))->resdom->restype = TEXTOID;
					((TargetEntry *)lfirst(prev_target))->resdom->restype = TEXTOID;
				}
				prev_target = lnext(prev_target);
			}
			union_list = lappend(union_list, qlist->qtrees[i]);
		}
		return union_list;
	}
	else
		return NIL;
} /* transformUnionClause() */
