/*-------------------------------------------------------------------------
 *
 * parse_clause.c--
 *	  handle clauses in parser
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_clause.c,v 1.27 1999/02/02 12:57:51 wieck Exp $
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



#define ORDER_CLAUSE 0
#define GROUP_CLAUSE 1

static char *clauseText[] = {"ORDER", "GROUP"};

static TargetEntry *
			findTargetlistEntry(ParseState *pstate, Node *node, List *tlist, int clause);

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

	/* This could only happen for multi-action rules */
	if (pstate->p_target_relation != NULL)
	{
		heap_close(pstate->p_target_relation);
	}

	pstate->p_target_rangetblentry = rte;
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
 *	findTargetlistEntry -
 *	  returns the Resdom in the target list matching the specified varname
 *	  and range. If none exist one is created.
 *
 *	  Rewritten for ver 6.4 to handle expressions in the GROUP/ORDER BY clauses.
 *	   - daveh@insightdist.com	1998-07-31
 *
 */
static TargetEntry *
findTargetlistEntry(ParseState *pstate, Node *node, List *tlist, int clause)
{
	List	   *l;
	int			rtable_pos = 0,
				target_pos = 0,
				targetlist_pos = 0;
	TargetEntry *target_result = NULL;
	Value	   *val = NULL;
	char	   *relname = NULL;
	char	   *name = NULL;
	Node	   *expr = NULL;
	int			relCnt = 0;

	/* Pull out some values before looping thru target list  */
	switch (nodeTag(node))
	{
		case T_Attr:
			relname = ((Attr *) node)->relname;
			val = (Value *) lfirst(((Attr *) node)->attrs);
			name = strVal(val);
			rtable_pos = refnameRangeTablePosn(pstate, relname, NULL);
			relCnt = length(pstate->p_rtable);
			break;

		case T_Ident:
			name = ((Ident *) node)->name;
			relCnt = length(pstate->p_rtable);
			break;

		case T_A_Const:
			val = &((A_Const *) node)->val;

			if (nodeTag(val) != T_Integer)
				elog(ERROR, "Illegal Constant in %s BY", clauseText[clause]);
			target_pos = intVal(val);
			break;

		case T_FuncCall:
		case T_A_Expr:
			expr = transformExpr(pstate, node, EXPR_COLUMN_FIRST);
			break;

		default:
			elog(ERROR, "Illegal %s BY node = %d", clauseText[clause], nodeTag(node));
	}

	/*
	 * Loop through target entries and try to match to node
	 */
	foreach(l, tlist)
	{
		TargetEntry *target = (TargetEntry *) lfirst(l);
		Resdom	   *resnode = target->resdom;
		Var		   *var = (Var *) target->expr;
		char	   *resname = resnode->resname;
		int			test_rtable_pos = var->varno;

		++targetlist_pos;

		switch (nodeTag(node))
		{
			case T_Attr:
				if (strcmp(resname, name) == 0 && rtable_pos == test_rtable_pos)
				{

					/*
					 * Check for only 1 table & ORDER BY -ambiguity does
					 * not matter here
					 */
					if (clause == ORDER_CLAUSE && relCnt == 1)
						return target;

					if (target_result != NULL)
						elog(ERROR, "%s BY '%s' is ambiguous", clauseText[clause], name);
					else
						target_result = target;
					/* Stay in loop to check for ambiguity */
				}
				break;

			case T_Ident:
				if (strcmp(resname, name) == 0)
				{

					/*
					 * Check for only 1 table & ORDER BY  -ambiguity does
					 * not matter here
					 */
					if (clause == ORDER_CLAUSE && relCnt == 1)
						return target;

					if (target_result != NULL)
						elog(ERROR, "%s BY '%s' is ambiguous", clauseText[clause], name);
					else
						target_result = target;
					/* Stay in loop to check for ambiguity	*/
				}
				break;

			case T_A_Const:
				if (target_pos == targetlist_pos)
				{
					/* Can't be ambigious and we got what we came for  */
					return target;
				}
				break;

			case T_FuncCall:
			case T_A_Expr:
				if (equal(expr, target->expr))
				{

					/*
					 * Check for only 1 table & ORDER BY  -ambiguity does
					 * not matter here
					 */
					if (clause == ORDER_CLAUSE)
						return target;

					if (target_result != NULL)
						elog(ERROR, "GROUP BY has ambiguous expression");
					else
						target_result = target;
				}
				break;

			default:
				elog(ERROR, "Illegal %s BY node = %d", clauseText[clause], nodeTag(node));
		}
	}

	/*
	 * If no matches, construct a new target entry which is appended to
	 * the end of the target list.	 This target is set to be  resjunk =
	 * TRUE so that it will not be projected into the final tuple.
	 */
	if (target_result == NULL)
	{
		switch (nodeTag(node))
		{
			case T_Attr:
				target_result = MakeTargetEntryIdent(pstate, node,
										 &((Attr *) node)->relname, NULL,
										 ((Attr *) node)->relname, TRUE);
				lappend(tlist, target_result);
				break;

			case T_Ident:
				target_result = MakeTargetEntryIdent(pstate, node,
										   &((Ident *) node)->name, NULL,
										   ((Ident *) node)->name, TRUE);
				lappend(tlist, target_result);
				break;

			case T_A_Const:

				/*
				 * If we got this far, then must have been an out-of-range
				 * column number
				 */
				elog(ERROR, "%s BY position %d is not in target list", clauseText[clause], target_pos);
				break;

			case T_FuncCall:
			case T_A_Expr:
				target_result = MakeTargetEntryExpr(pstate, "resjunk", expr, FALSE, TRUE);
				lappend(tlist, target_result);
				break;

			default:
				elog(ERROR, "Illegal %s BY node = %d", clauseText[clause], nodeTag(node));
				break;
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

		restarget = findTargetlistEntry(pstate, lfirst(grouplist), targetlist, GROUP_CLAUSE);

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

		restarget = findTargetlistEntry(pstate, sortby->node, targetlist, ORDER_CLAUSE);

		sortcl->resdom = resdom = restarget->resdom;

		/*
		 * if we have InvalidOid, then this is a NULL field and don't need
		 * to sort
		 */
		if (resdom->restype == InvalidOid)
			resdom->restype = INT4OID;

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
					 * We use equal() here because we are called for UNION
					 * from the optimizer, and at that point, the sort
					 * clause resdom pointers don't match the target list
					 * resdom pointers
					 */
					if (equal(sortcl->resdom, tlelt->resdom))
						break;
					s = lnext(s);
				}
				if (s == NIL)
				{
					/* not a member of the sortclauses yet */
					SortClause *sortcl = makeNode(SortClause);

					if (tlelt->resdom->restype == InvalidOid)
						tlelt->resdom->restype = INT4OID;

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
 *	are matched up to ensure correct types in the results.
 * The select clause parsing is done recursively, so the unions are evaluated
 *	right-to-left. One might want to look at all columns from all clauses before
 *	trying to coerce, but unless we keep track of the call depth we won't know
 *	when to do this because of the recursion.
 * Let's just try matching in pairs for now (right to left) and see if it works.
 * - thomas 1998-05-22
 */
List *
transformUnionClause(List *unionClause, List *targetlist)
{
	List	   *union_list = NIL;
	QueryTreeList *qlist;
	int			i;

	if (unionClause)
	{
		/* recursion */
		qlist = parse_analyze(unionClause, NULL);

		for (i = 0; i < qlist->len; i++)
		{
			List	   *prev_target = targetlist;
			List	   *next_target;

			if (length(targetlist) != length(qlist->qtrees[i]->targetList))
				elog(ERROR, "Each UNION clause must have the same number of columns");

			foreach(next_target, qlist->qtrees[i]->targetList)
			{
				Oid			itype;
				Oid			otype;

				otype = ((TargetEntry *) lfirst(prev_target))->resdom->restype;
				itype = ((TargetEntry *) lfirst(next_target))->resdom->restype;

				/* one or both is a NULL column? then don't convert... */
				if (otype == InvalidOid)
				{
					/* propagate a known type forward, if available */
					if (itype != InvalidOid)
						((TargetEntry *) lfirst(prev_target))->resdom->restype = itype;
#if FALSE
					else
					{
						((TargetEntry *) lfirst(prev_target))->resdom->restype = UNKNOWNOID;
						((TargetEntry *) lfirst(next_target))->resdom->restype = UNKNOWNOID;
					}
#endif
				}
				else if (itype == InvalidOid)
				{
				}
				/* they don't match in type? then convert... */
				else if (itype != otype)
				{
					Node	   *expr;

					expr = ((TargetEntry *) lfirst(next_target))->expr;
					expr = CoerceTargetExpr(NULL, expr, itype, otype);
					if (expr == NULL)
					{
						elog(ERROR, "Unable to transform %s to %s"
							 "\n\tEach UNION clause must have compatible target types",
							 typeidTypeName(itype),
							 typeidTypeName(otype));
					}
					((TargetEntry *) lfirst(next_target))->expr = expr;
					((TargetEntry *) lfirst(next_target))->resdom->restype = otype;
				}

				/* both are UNKNOWN? then evaluate as text... */
				else if (itype == UNKNOWNOID)
				{
					((TargetEntry *) lfirst(next_target))->resdom->restype = TEXTOID;
					((TargetEntry *) lfirst(prev_target))->resdom->restype = TEXTOID;
				}
				prev_target = lnext(prev_target);
			}
			union_list = lappend(union_list, qlist->qtrees[i]);
		}
		return union_list;
	}
	else
		return NIL;
}	/* transformUnionClause() */
