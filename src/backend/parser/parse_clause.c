/*-------------------------------------------------------------------------
 *
 * parse_clause.c
 *	  handle clauses in parser
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_clause.c,v 1.52 2000/02/15 03:37:47 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/heapam.h"
#include "miscadmin.h"
#include "optimizer/tlist.h"
#include "parse.h"
#include "nodes/makefuncs.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"

#define ORDER_CLAUSE 0
#define GROUP_CLAUSE 1
#define DISTINCT_ON_CLAUSE 2

static char *clauseText[] = {"ORDER BY", "GROUP BY", "DISTINCT ON"};

static TargetEntry *findTargetlistEntry(ParseState *pstate, Node *node,
										List *tlist, int clause);
static void parseFromClause(ParseState *pstate, List *frmList);
RangeTblEntry *transformTableEntry(ParseState *pstate, RangeVar *r);
static List *addTargetToSortList(TargetEntry *tle, List *sortlist,
								 List *targetlist, char *opname);
static bool exprIsInSortList(Node *expr, List *sortList, List *targetList);

#ifndef DISABLE_OUTER_JOINS
static Node *transformUsingClause(ParseState *pstate, List *using, List *left, List *right);
#endif


/*
 * makeRangeTable -
 *	  Build the initial range table from the FROM clause.
 */
void
makeRangeTable(ParseState *pstate, List *frmList)
{
	/* Currently, nothing to do except this: */
	parseFromClause(pstate, frmList);
}

/*
 * setTargetTable
 *	  Add the target relation of INSERT or UPDATE to the range table,
 *	  and make the special links to it in the ParseState.
 *
 *	  Note that the target is not marked as either inFromCl or inJoinSet.
 *	  For INSERT, we don't want the target to be joined to; it's a
 *	  destination of tuples, not a source.  For UPDATE/DELETE, we do
 *	  need to scan or join the target.  This will happen without the
 *	  inJoinSet flag because the planner's preprocess_targetlist()
 *	  adds the destination's CTID attribute to the targetlist, and
 *	  therefore the destination will be a referenced table even if
 *	  there is no other use of any of its attributes.  Tricky, eh?
 */
void
setTargetTable(ParseState *pstate, char *relname)
{
	RangeTblEntry *rte;
	int			sublevels_up;

	if ((refnameRangeTablePosn(pstate, relname, &sublevels_up) == 0)
		|| (sublevels_up != 0))
		rte = addRangeTableEntry(pstate, relname,
								 makeAttr(relname, NULL),
								 FALSE, FALSE, FALSE);
	else
		rte = refnameRangeTableEntry(pstate, relname);

	/* This could only happen for multi-action rules */
	if (pstate->p_target_relation != NULL)
		heap_close(pstate->p_target_relation, AccessShareLock);

	pstate->p_target_rangetblentry = rte;
	pstate->p_target_relation = heap_open(rte->relid, AccessShareLock);
	/* will close relation later, see analyze.c */
}


Node *
mergeInnerJoinQuals(ParseState *pstate, Node *clause);

Node *
mergeInnerJoinQuals(ParseState *pstate, Node *clause)
{
	A_Expr	   *expr = (A_Expr *) pstate->p_join_quals;

	if (expr == NULL)
		return clause;

	if (clause != NULL)
	{
		A_Expr	   *a = makeNode(A_Expr);

		a->oper = AND;
		a->opname = NULL;
		a->lexpr = (Node *) expr;
		a->rexpr = clause;
		expr = a;
	}

	/* Make sure that we don't do this twice... */
	pstate->p_join_quals = NULL;

	return (Node *) expr;
} /* mergeInnerJoinQuals() */

/*
 * transformWhereClause -
 *	  transforms the qualification and make sure it is of type Boolean
 */
Node *
transformWhereClause(ParseState *pstate, Node *clause)
{
	Node	   *qual;

	if (pstate->p_join_quals != NULL)
		clause = mergeInnerJoinQuals(pstate, clause);

	if (clause == NULL)
		return NULL;

	pstate->p_in_where_clause = true;
	qual = transformExpr(pstate, clause, EXPR_COLUMN_FIRST);
	pstate->p_in_where_clause = false;

	if (exprType(qual) != BOOLOID)
	{
		elog(ERROR, "WHERE clause must return type bool, not type %s",
			 typeidTypeName(exprType(qual)));
	}
	return qual;
}

#ifndef DISABLE_JOIN_SYNTAX
char *
AttrString(Attr *attr);

char *
AttrString(Attr *attr)
{
	Value *val;

	Assert(length(attr->attrs) == 1);

	val = lfirst(attr->attrs);

	Assert(IsA(val, String));

	return strVal(val);
}

List *
ListTableAsAttrs(ParseState *pstate, char *table);
List *
ListTableAsAttrs(ParseState *pstate, char *table)
{
	List *rlist = NULL;
	List *col;

	Attr *attr = expandTable(pstate, table, TRUE);
	foreach(col, attr->attrs)
	{
		Attr *a;
		a = makeAttr(table, strVal((Value *) col));
		rlist = lappend(rlist, a);
	}

	return rlist;
}

List *
makeUniqueAttrList(List *candidates, List *idents);
List *
makeUniqueAttrList(List *attrs, List *filter)
{
	List *result = NULL;
	List *candidate;

	foreach(candidate, attrs)
	{
		List *fmember;
		bool match = FALSE;
//		char *field;
		Attr *cattr = lfirst(candidate);

		Assert(IsA(cattr, Attr));
		Assert(length(cattr->attrs) == 1);

//		field = strVal(lfirst(ccol));
//		bool match = FALSE;

		foreach(fmember, filter)
		{
			Attr *fattr = lfirst(fmember);
			Assert(IsA(fattr, Attr));
			Assert(length(fattr->attrs) == 1);

			if (strcmp(strVal(lfirst(cattr->attrs)), strVal(lfirst(fattr->attrs))) == 0)
			{
				match = TRUE;
				break;
			}
		}

		if (!match)
			result = lappend(result, cattr);
	}

	return result;
}

List *
makeAttrList(Attr *attr);

List *
makeAttrList(Attr *attr)
{
	List *result = NULL;

	char *name = attr->relname;
	List *col;

	foreach (col, attr->attrs)
	{
		Attr *newattr = makeAttr(name, strVal((Value *) lfirst(col)));

		result = lappend(result, newattr);
	}

	return result;
}

/* ExpandAttrs()
 * Take an existing attribute node and return a list of attribute nodes
 * with one attribute name per node.
 */
List *
ExpandAttrs(Attr *attr);
List *
ExpandAttrs(Attr *attr)
{
	List *col;
	char *relname = attr->relname;
	List *rlist = NULL;

	Assert(attr != NULL);

	if ((attr->attrs == NULL) || (length(attr->attrs) <= 1))
		return lcons(attr, NIL);

	foreach(col, attr->attrs)
	{
		Attr *attr = lfirst(col);

		rlist = lappend(rlist, makeAttr(relname, AttrString(attr)));
	}

	return rlist;
}

/* transformUsingClause()
 * Take an ON or USING clause from a join expression and expand if necessary.
 */
static Node *
transformUsingClause(ParseState *pstate, List *usingList, List *leftList, List *rightList)
{
	A_Expr	   *expr = NULL;
	List	   *using;

	foreach(using, usingList)
	{
		List *col;
		A_Expr *e;

		Attr *uattr = lfirst(using);
		Attr *lattr = NULL, *rattr = NULL;

		/* find the first instances of this column in the shape list
		 * and the last table in the shape list...
		 */
		foreach (col, leftList)
		{
			Attr *attr = lfirst(col);

			if (strcmp(AttrString(attr), AttrString(uattr)) == 0)
			{
				lattr = attr;
				break;
			}
		}
		foreach (col, rightList)
		{
			Attr *attr = lfirst(col);

			if (strcmp(AttrString(attr), AttrString(uattr)) == 0)
			{
				rattr = attr;
				break;
			}
		}

		Assert((lattr != NULL) && (rattr != NULL));

		e = makeNode(A_Expr);
		e->oper = OP;
		e->opname = "=";
		e->lexpr = (Node *) lattr;
		e->rexpr = (Node *) rattr;

		if (expr != NULL)
		{
			A_Expr	   *a = makeNode(A_Expr);

			a->oper = AND;
			a->opname = NULL;
			a->lexpr = (Node *) expr;
			a->rexpr = (Node *) e;
			expr = a;
		}
		else
			expr = e;
	}

	return ((Node *) transformExpr(pstate, (Node *) expr, EXPR_COLUMN_FIRST));
} /* transformUsiongClause() */
#endif


RangeTblEntry *
transformTableEntry(ParseState *pstate, RangeVar *r)
{
	RelExpr    *baserel = r->relExpr;
	char	   *relname = baserel->relname;
#if 0
	char	   *refname;
	List	   *columns;
#endif
	RangeTblEntry *rte;

#if 0
	if (r->name != NULL)
		refname = r->name->relname;
	else
		refname = NULL;

	columns = ListTableAsAttrs(pstate, relname);

	/* alias might be specified... */
	if (r->name != NULL)
	{
#ifndef DISABLE_JOIN_SYNTAX
		if (length(columns) > 0)
		{
			if (length(r->name->attrs) > 0)
			{
				if (length(columns) != length(r->name->attrs))
					elog(ERROR, "'%s' has %d columns but %d %s specified",
						 relname, length(columns), length(r->name->attrs),
						 ((length(r->name->attrs) != 1)? "aliases": "alias"));

				aliasList = nconc(aliasList, r->name->attrs);
			}
			else
			{
				r->name->attrs = columns;

				aliasList = nconc(aliasList, r->name->attrs);
			}
		}
		else
		{
			elog(NOTICE, "transformTableEntry: column aliases not handled (internal error)");
		}
#else
		elog(ERROR, "Column aliases not yet supported");
#endif
	}
	else
	{
		refname = relname;
		aliasList = nconc(aliasList, columns);
	}
#endif

	if (r->name == NULL)
		r->name = makeAttr(relname, NULL);

	/*
	 * marks this entry to indicate it comes from the FROM clause. In SQL,
	 * the target list can only refer to range variables specified in the
	 * from clause but we follow the more powerful POSTQUEL semantics and
	 * automatically generate the range variable if not specified. However
	 * there are times we need to know whether the entries are legitimate.
	 *
	 * eg. select * from foo f where f.x = 1; will generate wrong answer if
	 * we expand * to foo.x.
	 */

	rte = addRangeTableEntry(pstate, relname, r->name,
							 baserel->inh, TRUE, TRUE);

	return rte;
} /* transformTableEntry() */


/*
 * parseFromClause -
 *	  turns the table references specified in the from-clause into a
 *	  range table. The range table may grow as we transform the expressions
 *	  in the target list. (Note that this happens because in POSTQUEL, we
 *	  allow references to relations not specified in the from-clause. We
 *	  also allow now as an extension.)
 *
 * The FROM clause can now contain JoinExpr nodes, which contain parsing info
 * for inner and outer joins. The USING clause must be expanded into a qualification
 * for an inner join at least, since that is compatible with the old syntax.
 * Not sure yet how to handle outer joins, but it will become clear eventually?
 * - thomas 1998-12-16
 */
static void
parseFromClause(ParseState *pstate, List *frmList)
{
//	List *shape, *alias;
//	Node **qual;
//	char *lname, *rname;

	List *fl;

	foreach(fl, frmList)
	{
		Node	   *n = lfirst(fl);

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

		/* Plain vanilla inner join, just like we've always had? */
		if (IsA(n, RangeVar))
		{
			transformTableEntry(pstate, (RangeVar *) n);
		}

		/* A newfangled join expression? */
		else if (IsA(n, JoinExpr))
		{
#ifndef DISABLE_JOIN_SYNTAX
//			char			   *lname, *rname;
			RangeTblEntry	   *l_rte, *r_rte;
			Attr			   *l_name, *r_name;
			JoinExpr *j = (JoinExpr *) n;

			if (j->alias != NULL)
				elog(ERROR, "JOIN table aliases are not supported");

			/* nested join? then handle the left one first... */
			if (IsA(j->larg, JoinExpr))
			{
				parseFromClause(pstate, lcons(j->larg, NIL));
				l_name = ((JoinExpr *)j->larg)->alias;
			}
			else
			{
				Assert(IsA(j->larg, RangeVar));
				l_rte = transformTableEntry(pstate, (RangeVar *) j->larg);
				l_name = expandTable(pstate, l_rte->ref->relname, TRUE);
			}

			if (IsA(j->rarg, JoinExpr))
			{
//				elog(ERROR, "Nested JOINs are not yet supported");
				parseFromClause(pstate, lcons(j->rarg, NIL));
				l_name = ((JoinExpr *)j->larg)->alias;
			}
			else
			{
				Assert(IsA(j->rarg, RangeVar));
				r_rte = transformTableEntry(pstate, (RangeVar *) j->rarg);
				r_name = expandTable(pstate, r_rte->ref->relname, TRUE);
			}

			/* Natural join does not explicitly specify columns; must generate columns to join.
			 * Need to run through the list of columns from each table or join result
			 * and match up the column names. Use the first table, and check every
			 * column in the second table for a match.
			 */
			if (j->isNatural)
			{
				List *lx, *rx;
				List *rlist = NULL;

				foreach(lx, l_name->attrs)
				{
					Ident *id = NULL;
					Value *l_col = lfirst(lx);
					Assert(IsA(l_col, String));

					foreach(rx, r_name->attrs)
					{
						Value *r_col = lfirst(rx);
						Assert(IsA(r_col, String));

//						if (equal(l_col, r_col))
						if (strcmp(strVal(l_col), strVal(r_col)) == 0)
						{
							id = (Ident *) makeNode(Ident);
							id->name = strVal(l_col);
							break;
						}
					}

					/* right column matched? then keep as join column... */
					if (id != NULL)
						rlist = lappend(rlist, id);
				}
				j->quals = rlist;

				printf("NATURAL JOIN columns are %s\n", nodeToString(rlist));
			}

			if (j->jointype == INNER_P)
			{
				/* CROSS JOIN */
				if (j->quals == NULL)
				{
					printf("CROSS JOIN...\n");
				}

				/* JOIN/USING
				 * This is an inner join, so rip apart the join node and
				 * transform into a traditional FROM list. NATURAL JOIN
				 * and JOIN USING both change the shape of the result.
				 * Need to generate a list of result columns to use for
				 * target list expansion and validation.
				 */
				else if (IsA(j->quals, List))
				{
					/*
					 * List of Ident nodes means column names from a real USING
					 * clause. Determine the shape of the joined table.
					 */
//					List *ltable, *rtable;
					List *ucols, *ucol;
					List *shape = NULL;
					List *alias = NULL;
					List *l_shape, *r_shape;

					List *l_cols = makeAttrList(l_name);
					List *r_cols = makeAttrList(r_name);

					printf("USING input tables are:\n %s\n %s\n",
						   nodeToString(l_name), nodeToString(r_name));

					printf("USING expanded tables are:\n %s\n %s\n",
						   nodeToString(l_cols), nodeToString(r_cols));

					/* Columns from the USING clause... */
					ucols = (List *)j->quals;
					foreach(ucol, ucols)
					{
						List *col;
						Attr *l_attr = NULL, *r_attr = NULL;
						Ident *id = lfirst(ucol);

						Attr *attr = makeAttr("", id->name);

						foreach(col, l_cols)
						{
							attr = lfirst(col);
							if (strcmp(AttrString(attr), id->name) == 0)
							{
								l_attr = attr;
								break;
							}
						}

						foreach(col, r_cols)
						{
							attr = lfirst(col);
							if (strcmp(AttrString(attr), id->name) == 0)
							{
								r_attr = attr;
								break;
							}
						}

						if (l_attr == NULL)
							elog(ERROR, "USING column '%s' not found in table '%s'",
								 id->name, l_name->relname);
						if (r_attr == NULL)
							elog(ERROR, "USING column '%s' not found in table '%s'",
								 id->name, r_name->relname);

						shape = lappend(shape, l_attr);
						alias = lappend(alias, makeAttr("", AttrString(l_attr)));
					}
					printf("JOIN/USING join columns are %s\n", nodeToString(shape));

					/* Remaining columns from the left side... */
					l_shape = makeUniqueAttrList(makeAttrList(l_name), shape);

					printf("JOIN/USING left columns are %s\n", nodeToString(l_shape));

					r_shape = makeUniqueAttrList(makeAttrList(r_name), shape);

					printf("JOIN/USING right columns are %s\n", nodeToString(r_shape));

					printf("JOIN/USING input quals are %s\n", nodeToString(j->quals));

					j->quals = (List *) transformUsingClause(pstate, shape, l_cols, r_cols);

					printf("JOIN/USING transformed quals are %s\n", nodeToString(j->quals));

					alias = nconc(nconc(alias, listCopy(l_shape)), listCopy(r_shape));
					shape = nconc(nconc(shape, l_shape), r_shape);

					printf("JOIN/USING shaped table is %s\n", nodeToString(shape));
					printf("JOIN/USING alias list is %s\n", nodeToString(alias));

					pstate->p_shape = shape;
					pstate->p_alias = alias;
				}

				/* otherwise, must be an expression from an ON clause... */
				else
				{
					j->quals = (List *) lcons(j->quals, NIL);
				}

				pstate->p_join_quals = (Node *) j->quals;

#if 0
				if (qual == NULL)
					elog(ERROR, "JOIN/ON not supported in this context");

				printf("Table aliases are %s\n", nodeToString(*aliasList));
#endif

#if 0
				if (*qual == NULL)
				{
#endif

#if 0
					/* merge qualified join clauses... */
				if (j->quals != NULL)
				{
					if (*qual != NULL)
					{
						A_Expr	   *a = makeNode(A_Expr);

						a->oper = AND;
						a->opname = NULL;
						a->lexpr = (Node *) *qual;
						a->rexpr = (Node *) j->quals;

						*qual = (Node *)a;
					}
					else
					{
						*qual = (Node *)j->quals;
					}
				}
#endif

#if 0
				}
				else
				{
					elog(ERROR, "Multiple JOIN/ON clauses not handled (internal error)");
					*qual = lappend(*qual, j->quals);
				}
#endif

				/*
				 * if we are transforming this node back into a FROM list,
				 * then we will need to replace the node with two nodes.
				 * Will need access to the previous list item to change
				 * the link pointer to reference these new nodes. Try
				 * accumulating and returning a new list.
				 * - thomas 1999-01-08 Not doing this yet though!
				 */

			}
			else if ((j->jointype == LEFT)
					 || (j->jointype == RIGHT)
					 || (j->jointype == FULL))
				elog(ERROR, "OUTER JOIN is not yet supported");
			else
				elog(ERROR, "Unrecognized JOIN clause; tag is %d (internal error)",
					 j->jointype);
#else
			elog(ERROR, "JOIN expressions are not yet implemented");
#endif
		}
		else
			elog(ERROR, "parseFromClause: unexpected FROM clause node (internal error)"
				 "\n\t%s", nodeToString(n));
	}
} /* parseFromClause() */


/*
 *	findTargetlistEntry -
 *	  Returns the targetlist entry matching the given (untransformed) node.
 *	  If no matching entry exists, one is created and appended to the target
 *	  list as a "resjunk" node.
 *
 * node		the ORDER BY, GROUP BY, or DISTINCT ON expression to be matched
 * tlist	the existing target list (NB: this cannot be NIL, which is a
 *			good thing since we'd be unable to append to it...)
 * clause	identifies clause type for error messages.
 */
static TargetEntry *
findTargetlistEntry(ParseState *pstate, Node *node, List *tlist, int clause)
{
	TargetEntry *target_result = NULL;
	List	   *tl;
	Node	   *expr;

	/*----------
	 * Handle two special cases as mandated by the SQL92 spec:
	 *
	 * 1. ORDER/GROUP BY ColumnName
	 *    For a bare identifier, we search for a matching column name
	 *	  in the existing target list.  Multiple matches are an error
	 *	  unless they refer to identical values; for example,
	 *	  we allow  SELECT a, a FROM table ORDER BY a
	 *	  but not   SELECT a AS b, b FROM table ORDER BY b
	 *	  If no match is found, we fall through and treat the identifier
	 *	  as an expression.
	 *
	 * 2. ORDER/GROUP BY IntegerConstant
	 *	  This means to use the n'th item in the existing target list.
	 *	  Note that it would make no sense to order/group by an actual
	 *	  constant, so this does not create a conflict with our extension
	 *	  to order/group by an expression.
	 *
	 * Note that pre-existing resjunk targets must not be used in either case.
	 *----------
	 */
	if (IsA(node, Ident) && ((Ident *) node)->indirection == NIL)
	{
		char	   *name = ((Ident *) node)->name;
		foreach(tl, tlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(tl);
			Resdom	   *resnode = tle->resdom;

			if (!resnode->resjunk &&
				strcmp(resnode->resname, name) == 0)
			{
				if (target_result != NULL)
				{
					if (! equal(target_result->expr, tle->expr))
						elog(ERROR, "%s '%s' is ambiguous",
							 clauseText[clause], name);
				}
				else
					target_result = tle;
				/* Stay in loop to check for ambiguity */
			}
		}
		if (target_result != NULL)
			return target_result; /* return the first match */
	}
	if (IsA(node, A_Const))
	{
		Value	   *val = &((A_Const *) node)->val;
		int			targetlist_pos = 0;
		int			target_pos;

		if (! IsA(val, Integer))
			elog(ERROR, "Non-integer constant in %s", clauseText[clause]);
		target_pos = intVal(val);
		foreach(tl, tlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(tl);
			Resdom	   *resnode = tle->resdom;

			if (!resnode->resjunk)
			{
				if (++targetlist_pos == target_pos)
					return tle;	/* return the unique match */
			}
		}
		elog(ERROR, "%s position %d is not in target list",
			 clauseText[clause], target_pos);
	}

	/*
	 * Otherwise, we have an expression (this is a Postgres extension
	 * not found in SQL92).  Convert the untransformed node to a
	 * transformed expression, and search for a match in the tlist.
	 * NOTE: it doesn't really matter whether there is more than one
	 * match.  Also, we are willing to match a resjunk target here,
	 * though the above cases must ignore resjunk targets.
	 */
	expr = transformExpr(pstate, node, EXPR_COLUMN_FIRST);

	foreach(tl, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tl);

		if (equal(expr, tle->expr))
			return tle;
	}

	/*
	 * If no matches, construct a new target entry which is appended to
	 * the end of the target list.  This target is given resjunk = TRUE
	 * so that it will not be projected into the final tuple.
	 */
	target_result = transformTargetEntry(pstate, node, expr, NULL, true);
	lappend(tlist, target_result);

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
			   *gl;

	foreach(gl, grouplist)
	{
		TargetEntry *tle;

		tle = findTargetlistEntry(pstate, lfirst(gl),
								  targetlist, GROUP_CLAUSE);

		/* avoid making duplicate grouplist entries */
		if (! exprIsInSortList(tle->expr, glist, targetlist))
		{
			GroupClause *grpcl = makeNode(GroupClause);

			grpcl->tleSortGroupRef = assignSortGroupRef(tle, targetlist);

			grpcl->sortop = oprid(oper("<",
									   tle->resdom->restype,
									   tle->resdom->restype, false));

			glist = lappend(glist, grpcl);
		}
	}

	return glist;
}

/*
 * transformSortClause -
 *	  transform an ORDER BY clause
 */
List *
transformSortClause(ParseState *pstate,
					List *orderlist,
					List *targetlist)
{
	List	   *sortlist = NIL;
	List	   *olitem;

	foreach(olitem, orderlist)
	{
		SortGroupBy	   *sortby = lfirst(olitem);
		TargetEntry	   *tle;

		tle = findTargetlistEntry(pstate, sortby->node,
								  targetlist, ORDER_CLAUSE);

		sortlist = addTargetToSortList(tle, sortlist, targetlist,
									   sortby->useOp);
	}

	return sortlist;
}

/*
 * transformDistinctClause -
 *	  transform a DISTINCT or DISTINCT ON clause
 *
 * Since we may need to add items to the query's sortClause list, that list
 * is passed by reference.  We might also need to add items to the query's
 * targetlist, but we assume that cannot be empty initially, so we can
 * lappend to it even though the pointer is passed by value.
 */
List *
transformDistinctClause(ParseState *pstate, List *distinctlist,
						List *targetlist, List **sortClause)
{
	List	   *result = NIL;
	List	   *slitem;
	List	   *dlitem;

	/* No work if there was no DISTINCT clause */
	if (distinctlist == NIL)
		return NIL;

	if (lfirst(distinctlist) == NIL)
	{
		/* We had SELECT DISTINCT */

		/*
		 * All non-resjunk elements from target list that are not already
		 * in the sort list should be added to it.  (We don't really care
		 * what order the DISTINCT fields are checked in, so we can leave
		 * the user's ORDER BY spec alone, and just add additional sort keys
		 * to it to ensure that all targetlist items get sorted.)
		 */
		*sortClause = addAllTargetsToSortList(*sortClause, targetlist);

		/*
		 * Now, DISTINCT list consists of all non-resjunk sortlist items.
		 * Actually, all the sortlist items had better be non-resjunk!
		 * Otherwise, user wrote SELECT DISTINCT with an ORDER BY item
		 * that does not appear anywhere in the SELECT targetlist, and
		 * we can't implement that with only one sorting pass...
		 */
		foreach(slitem, *sortClause)
		{
			SortClause *scl = (SortClause *) lfirst(slitem);
			TargetEntry *tle = get_sortgroupclause_tle(scl, targetlist);

			if (tle->resdom->resjunk)
				elog(ERROR, "For SELECT DISTINCT, ORDER BY expressions must appear in target list");
			else
				result = lappend(result, copyObject(scl));
		}
	}
	else
	{
		/* We had SELECT DISTINCT ON (expr, ...) */

		/*
		 * If the user writes both DISTINCT ON and ORDER BY, then the two
		 * expression lists must match (until one or the other runs out).
		 * Otherwise the ORDER BY requires a different sort order than the
		 * DISTINCT does, and we can't implement that with only one sort pass
		 * (and if we do two passes, the results will be rather unpredictable).
		 * However, it's OK to have more DISTINCT ON expressions than ORDER BY
		 * expressions; we can just add the extra DISTINCT values to the sort
		 * list, much as we did above for ordinary DISTINCT fields.
		 *
		 * Actually, it'd be OK for the common prefixes of the two lists to
		 * match in any order, but implementing that check seems like more
		 * trouble than it's worth.
		 */
		List	   *nextsortlist = *sortClause;

		foreach(dlitem, distinctlist)
		{
			TargetEntry *tle;

			tle = findTargetlistEntry(pstate, lfirst(dlitem),
									  targetlist, DISTINCT_ON_CLAUSE);

			if (nextsortlist != NIL)
			{
				SortClause *scl = (SortClause *) lfirst(nextsortlist);

				if (tle->resdom->ressortgroupref != scl->tleSortGroupRef)
					elog(ERROR, "SELECT DISTINCT ON expressions must match initial ORDER BY expressions");
				result = lappend(result, copyObject(scl));
				nextsortlist = lnext(nextsortlist);
			}
			else
			{
				*sortClause = addTargetToSortList(tle, *sortClause,
												  targetlist, NULL);
				/* Probably, the tle should always have been added at the
				 * end of the sort list ... but search to be safe.
				 */
				foreach(slitem, *sortClause)
				{
					SortClause *scl = (SortClause *) lfirst(slitem);

					if (tle->resdom->ressortgroupref == scl->tleSortGroupRef)
					{
						result = lappend(result, copyObject(scl));
						break;
					}
				}
				if (slitem == NIL)
					elog(ERROR, "transformDistinctClause: failed to add DISTINCT ON clause to target list");
			}
		}
	}

	return result;
}

/*
 * addAllTargetsToSortList
 *		Make sure all non-resjunk targets in the targetlist are in the
 *		ORDER BY list, adding the not-yet-sorted ones to the end of the list.
 *		This is typically used to help implement SELECT DISTINCT.
 *
 * Returns the updated ORDER BY list.
 */
List *
addAllTargetsToSortList(List *sortlist, List *targetlist)
{
	List	   *i;

	foreach(i, targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(i);

		if (! tle->resdom->resjunk)
			sortlist = addTargetToSortList(tle, sortlist, targetlist, NULL);
	}
	return sortlist;
}

/*
 * addTargetToSortList
 *		If the given targetlist entry isn't already in the ORDER BY list,
 *		add it to the end of the list, using the sortop with given name
 *		or any available sort operator if opname == NULL.
 *
 * Returns the updated ORDER BY list.
 */
static List *
addTargetToSortList(TargetEntry *tle, List *sortlist, List *targetlist,
					char *opname)
{
	/* avoid making duplicate sortlist entries */
	if (! exprIsInSortList(tle->expr, sortlist, targetlist))
	{
		SortClause *sortcl = makeNode(SortClause);

		sortcl->tleSortGroupRef = assignSortGroupRef(tle, targetlist);

		if (opname)
			sortcl->sortop = oprid(oper(opname,
										tle->resdom->restype,
										tle->resdom->restype, false));
		else
			sortcl->sortop = any_ordering_op(tle->resdom->restype);

		sortlist = lappend(sortlist, sortcl);
	}
	return sortlist;
}

/*
 * assignSortGroupRef
 *	  Assign the targetentry an unused ressortgroupref, if it doesn't
 *	  already have one.  Return the assigned or pre-existing refnumber.
 *
 * 'tlist' is the targetlist containing (or to contain) the given targetentry.
 */
Index
assignSortGroupRef(TargetEntry *tle, List *tlist)
{
	Index		maxRef;
	List	   *l;

	if (tle->resdom->ressortgroupref)		/* already has one? */
		return tle->resdom->ressortgroupref;

	/* easiest way to pick an unused refnumber: max used + 1 */
	maxRef = 0;
	foreach(l, tlist)
	{
		Index	ref = ((TargetEntry *) lfirst(l))->resdom->ressortgroupref;

		if (ref > maxRef)
			maxRef = ref;
	}
	tle->resdom->ressortgroupref = maxRef + 1;
	return tle->resdom->ressortgroupref;
}

/*
 * exprIsInSortList
 *		Is the given expression already in the sortlist?
 *		Note we will say 'yes' if it is equal() to any sortlist item,
 *		even though that might be a different targetlist member.
 *
 * Works for both SortClause and GroupClause lists.
 */
static bool
exprIsInSortList(Node *expr, List *sortList, List *targetList)
{
	List	   *i;

	foreach(i, sortList)
	{
		SortClause *scl = (SortClause *) lfirst(i);

		if (equal(expr, get_sortgroupclause_expr(scl, targetList)))
			return true;
	}
	return false;
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
#ifdef NOT_USED
static List *
transformUnionClause(List *unionClause, List *targetlist)
{
	List	   *union_list = NIL;
	List	   *qlist,
			   *qlist_item;

	if (unionClause)
	{
		/* recursion */
		qlist = parse_analyze(unionClause, NULL);

		foreach(qlist_item, qlist)
		{
			Query	   *query = (Query *) lfirst(qlist_item);
			List	   *prev_target = targetlist;
			List	   *next_target;
			int			prev_len = 0,
						next_len = 0;

			foreach(prev_target, targetlist)
				if (!((TargetEntry *) lfirst(prev_target))->resdom->resjunk)
				prev_len++;

			foreach(next_target, query->targetList)
				if (!((TargetEntry *) lfirst(next_target))->resdom->resjunk)
				next_len++;

			if (prev_len != next_len)
				elog(ERROR, "Each UNION clause must have the same number of columns");

			foreach(next_target, query->targetList)
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
					expr = CoerceTargetExpr(NULL, expr, itype, otype, -1);
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
			union_list = lappend(union_list, query);
		}
		return union_list;
	}
	else
		return NIL;
}
#endif
