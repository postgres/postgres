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
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_clause.c,v 1.50 2000/01/26 05:56:42 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "optimizer/tlist.h"
#include "parse.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"


#define ORDER_CLAUSE 0
#define GROUP_CLAUSE 1

static char *clauseText[] = {"ORDER", "GROUP"};

static TargetEntry *findTargetlistEntry(ParseState *pstate, Node *node,
										List *tlist, int clause,
										char *uniqFlag);
static void parseFromClause(ParseState *pstate, List *frmList, Node **qual);
static char	*transformTableEntry(ParseState *pstate, RangeVar *r);
static List *addTargetToSortList(TargetEntry *tle, List *sortlist,
								 List *targetlist, char *opname);
static bool exprIsInSortList(Node *expr, List *sortList, List *targetList);

#ifdef ENABLE_OUTER_JOINS
static Node *transformUsingClause(ParseState *pstate, List *onList,
								  char *lname, char *rname);
#endif



/*
 * makeRangeTable -
 *	  Build the initial range table from the FROM clause.
 */
void
makeRangeTable(ParseState *pstate, List *frmList, Node **qual)
{
	/* Currently, nothing to do except this: */
	parseFromClause(pstate, frmList, qual);
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
		rte = addRangeTableEntry(pstate, relname, relname,
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

/*
 * transformWhereClause -
 *	  transforms the qualification and make sure it is of type Boolean
 *
 * Now accept an additional argument, which is a qualification derived
 * from the JOIN/ON or JOIN/USING syntax.
 * - thomas 1998-12-16
 */
Node *
transformWhereClause(ParseState *pstate, Node *a_expr, Node *o_expr)
{
	A_Expr	   *expr;
	Node	   *qual;

	if ((a_expr == NULL) && (o_expr == NULL))
		return NULL;			/* no qualifiers */

	if ((a_expr != NULL) && (o_expr != NULL))
	{
		A_Expr	   *a = makeNode(A_Expr);

		a->oper = AND;
		a->opname = NULL;
		a->lexpr = o_expr;
		a->rexpr = a_expr;
		expr = a;
	}
	else if (o_expr != NULL)
		expr = (A_Expr *) o_expr;
	else
		expr = (A_Expr *) a_expr;

	pstate->p_in_where_clause = true;
	qual = transformExpr(pstate, (Node *) expr, EXPR_COLUMN_FIRST);
	pstate->p_in_where_clause = false;

	if (exprType(qual) != BOOLOID)
	{
		elog(ERROR, "WHERE clause must return type bool, not type %s",
			 typeidTypeName(exprType(qual)));
	}
	return qual;
}

#ifdef ENABLE_OUTER_JOINS
static Attr *
makeAttr(char *relname, char *attname)
{
	Attr	   *a = makeNode(Attr);

	a->relname = relname;
	a->paramNo = NULL;
	a->attrs = lcons(makeString(attname), NIL);
	a->indirection = NULL;

	return a;
}
#endif

#ifdef ENABLE_OUTER_JOINS
/* transformUsingClause()
 * Take an ON or USING clause from a join expression and expand if necessary.
 */
static Node *
transformUsingClause(ParseState *pstate, List *onList, char *lname, char *rname)
{
	A_Expr	   *expr = NULL;
	List	   *on;
	Node	   *qual;

	foreach(on, onList)
	{
		qual = lfirst(on);

		/*
		 * Ident node means it is just a column name from a real USING
		 * clause...
		 */
		if (IsA(qual, Ident))
		{
			Ident	   *i = (Ident *) qual;
			Attr	   *lattr = makeAttr(lname, i->name);
			Attr	   *rattr = makeAttr(rname, i->name);
			A_Expr	   *e = makeNode(A_Expr);

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

		/* otherwise, we have an expression from an ON clause... */
		else
		{
			if (expr != NULL)
			{
				A_Expr	   *a = makeNode(A_Expr);

				a->oper = AND;
				a->opname = NULL;
				a->lexpr = (Node *) expr;
				a->rexpr = (Node *) qual;
				expr = a;
			}
			else
				expr = (A_Expr *) qual;
		}
	}
	return ((Node *) transformExpr(pstate, (Node *) expr, EXPR_COLUMN_FIRST));
}

#endif

static char *
transformTableEntry(ParseState *pstate, RangeVar *r)
{
	RelExpr    *baserel = r->relExpr;
	char	   *relname = baserel->relname;
	char	   *refname = r->name;
	RangeTblEntry *rte;

	if (refname == NULL)
		refname = relname;

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

	rte = addRangeTableEntry(pstate, relname, refname,
							 baserel->inh, TRUE, TRUE);

	return refname;
}

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
parseFromClause(ParseState *pstate, List *frmList, Node **qual)
{
	List	   *fl;

	if (qual != NULL)
		*qual = NULL;

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
		if (IsA(n, RangeVar))
			transformTableEntry(pstate, (RangeVar *) n);
		else if (IsA(n, JoinExpr))
		{
			JoinExpr   *j = (JoinExpr *) n;

#ifdef ENABLE_OUTER_JOINS
			char	   *lname = transformTableEntry(pstate, (RangeVar *) j->larg);

#endif
			char	   *rname;

			if (IsA((Node *) j->rarg, RangeVar))
				rname = transformTableEntry(pstate, (RangeVar *) j->rarg);
			else
				elog(ERROR, "Nested JOINs are not yet supported");

#ifdef ENABLE_OUTER_JOINS
			if (j->jointype == INNER_P)
			{

				/*
				 * This is an inner join, so rip apart the join node and
				 * transform into a traditional FROM list. NATURAL JOIN
				 * and USING clauses both change the shape of the result.
				 * Need to generate a list of result columns to use for
				 * target list expansion and validation. Not doing this
				 * yet though!
				 */
				if (IsA(j->quals, List))
					j->quals = lcons(transformUsingClause(pstate, (List *) j->quals, lname, rname), NIL);

				if (qual == NULL)
					elog(ERROR, "JOIN/ON not supported in this context");

				if (*qual == NULL)
					*qual = lfirst(j->quals);
				else
					elog(ERROR, "Multiple JOIN/ON clauses not handled (internal error)");

				/*
				 * if we are transforming this node back into a FROM list,
				 * then we will need to replace the node with two nodes.
				 * Will need access to the previous list item to change
				 * the link pointer to reference these new nodes. Try
				 * accumulating and returning a new list. - thomas
				 * 1999-01-08 Not doing this yet though!
				 */

			}
			else if ((j->jointype == LEFT)
					 || (j->jointype == RIGHT)
					 || (j->jointype == FULL))
				elog(ERROR, "OUTER JOIN is not implemented");
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
}


/*
 *	findTargetlistEntry -
 *	  Returns the targetlist entry matching the given (untransformed) node.
 *	  If no matching entry exists, one is created and appended to the target
 *	  list as a "resjunk" node.
 *
 * node		the ORDER BY or GROUP BY expression to be matched
 * tlist	the existing target list (NB: this cannot be NIL, which is a
 *			good thing since we'd be unable to append to it...)
 * clause	identifies clause type for error messages.
 */
static TargetEntry *
findTargetlistEntry(ParseState *pstate, Node *node, List *tlist, int clause,
					char *uniqueFlag)
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
						elog(ERROR, "%s BY '%s' is ambiguous",
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

		if (nodeTag(val) != T_Integer)
			elog(ERROR, "Non-integer constant in %s BY", clauseText[clause]);
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
		elog(ERROR, "%s BY position %d is not in target list",
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
	 * the end of the target list.	 This target is set to be  resjunk =
	 * TRUE so that it will not be projected into the final tuple.
	 */
	if(clause == ORDER_CLAUSE && uniqueFlag) {
		elog(ERROR, "ORDER BY columns must appear in SELECT DISTINCT target list");
	}

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
								  targetlist, GROUP_CLAUSE, NULL);

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
 *	  transform an Order By clause
 *
 */
List *
transformSortClause(ParseState *pstate,
					List *orderlist,
					List *targetlist,
					char *uniqueFlag)
{
	List	   *sortlist = NIL;
	List	   *olitem;

	/* Transform all the explicit ORDER BY clauses */

	foreach(olitem, orderlist)
	{
		SortGroupBy	   *sortby = lfirst(olitem);
		TargetEntry	   *tle;

		tle = findTargetlistEntry(pstate, sortby->node,
								  targetlist, ORDER_CLAUSE, uniqueFlag);

		sortlist = addTargetToSortList(tle, sortlist, targetlist,
									   sortby->useOp);
	}

	/* If we have a DISTINCT clause, add any necessary entries to
	 * the sortlist to ensure that all the DISTINCT columns will be
	 * sorted.  A subsequent UNIQUE pass will then do the right thing.
	 */

	if (uniqueFlag)
	{
		if (uniqueFlag[0] == '*')
		{
			/*
			 * concatenate all elements from target list that are not
			 * already in the sortby list
			 */
			sortlist = addAllTargetsToSortList(sortlist, targetlist);
		}
		else
		{
			TargetEntry *tle = NULL;
			char	   *uniqueAttrName = uniqueFlag;
			List	   *i;

			/* only create sort clause with the specified unique attribute */
			foreach(i, targetlist)
			{
				tle = (TargetEntry *) lfirst(i);
				if (strcmp(tle->resdom->resname, uniqueAttrName) == 0)
					break;
			}
			if (i == NIL)
				elog(ERROR, "All fields in the UNIQUE ON clause must appear in the target list");

			sortlist = addTargetToSortList(tle, sortlist, targetlist, NULL);
		}
	}

	return sortlist;
}

/*
 * addAllTargetsToSortList
 *		Make sure all targets in the targetlist are in the ORDER BY list,
 *		adding the not-yet-sorted ones to the end of the list.
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
