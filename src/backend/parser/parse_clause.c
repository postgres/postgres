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
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_clause.c,v 1.72 2000/11/12 00:37:00 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "nodes/makefuncs.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/analyze.h"
#include "parser/parse.h"
#include "parser/parsetree.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"

#define ORDER_CLAUSE 0
#define GROUP_CLAUSE 1
#define DISTINCT_ON_CLAUSE 2

static char *clauseText[] = {"ORDER BY", "GROUP BY", "DISTINCT ON"};

static void extractUniqueColumns(List *common_colnames,
								 List *src_colnames, List *src_colvars,
								 List **res_colnames, List **res_colvars);
static Node *transformJoinUsingClause(ParseState *pstate,
									  List *leftVars, List *rightVars);
static Node *transformJoinOnClause(ParseState *pstate, JoinExpr *j,
								   List *containedRels);
static RangeTblRef *transformTableEntry(ParseState *pstate, RangeVar *r);
static RangeTblRef *transformRangeSubselect(ParseState *pstate,
											RangeSubselect *r);
static Node *transformFromClauseItem(ParseState *pstate, Node *n,
									 List **containedRels);
static TargetEntry *findTargetlistEntry(ParseState *pstate, Node *node,
					List *tlist, int clause);
static List *addTargetToSortList(TargetEntry *tle, List *sortlist,
					List *targetlist, char *opname);
static bool exprIsInSortList(Node *expr, List *sortList, List *targetList);


/*
 * makeRangeTable -
 *	  Build the initial range table from the FROM clause.
 *
 * The range table constructed here may grow as we transform the expressions
 * in the query's quals and target list. (Note that this happens because in
 * POSTQUEL, we allow references to relations not specified in the
 * from-clause.  PostgreSQL keeps this extension to standard SQL.)
 *
 * Note: we assume that pstate's p_rtable and p_joinlist lists were
 * initialized to NIL when the pstate was created.  We will add onto
 * any entries already present --- this is needed for rule processing!
 */
void
makeRangeTable(ParseState *pstate, List *frmList)
{
	List	   *fl;

	/*
	 * The grammar will have produced a list of RangeVars, RangeSubselects,
	 * and/or JoinExprs. Transform each one, and then add it to the joinlist.
	 */
	foreach(fl, frmList)
	{
		Node	   *n = lfirst(fl);
		List	   *containedRels;

		n = transformFromClauseItem(pstate, n, &containedRels);
		pstate->p_joinlist = lappend(pstate->p_joinlist, n);
	}
}

/*
 * lockTargetTable
 *	  Find the target relation of INSERT/UPDATE/DELETE and acquire write
 *	  lock on it.  This must be done before building the range table,
 *	  in case the target is also mentioned as a source relation --- we
 *	  want to be sure to grab the write lock before any read lock.
 *
 * The ParseState's link to the target relcache entry is also set here.
 */
void
lockTargetTable(ParseState *pstate, char *relname)
{
	/* Close old target; this could only happen for multi-action rules */
	if (pstate->p_target_relation != NULL)
		heap_close(pstate->p_target_relation, NoLock);
	pstate->p_target_relation = NULL;
	pstate->p_target_rangetblentry = NULL; /* setTargetTable will set this */

	/*
	 * Open target rel and grab suitable lock (which we will hold till
	 * end of transaction).
	 *
	 * analyze.c will eventually do the corresponding heap_close(),
	 * but *not* release the lock.
	 */
	pstate->p_target_relation = heap_openr(relname, RowExclusiveLock);
}

/*
 * setTargetTable
 *	  Add the target relation of INSERT/UPDATE/DELETE to the range table,
 *	  and make the special links to it in the ParseState.
 *
 *	  inJoinSet says whether to add the target to the join list.
 *	  For INSERT, we don't want the target to be joined to; it's a
 *	  destination of tuples, not a source.	For UPDATE/DELETE, we do
 *	  need to scan or join the target.
 */
void
setTargetTable(ParseState *pstate, char *relname, bool inh, bool inJoinSet)
{
	RangeTblEntry *rte;

	/* look for relname only at current nesting level... */
	if (refnameRangeTablePosn(pstate, relname, NULL) == 0)
	{
		rte = addRangeTableEntry(pstate, relname, NULL, inh, false);
		/*
		 * Since the rel wasn't in the rangetable already, it's not being
		 * read; override addRangeTableEntry's default checkForRead.
		 *
		 * If we find an explicit reference to the rel later during
		 * parse analysis, scanRTEForColumn will change checkForRead
		 * to 'true' again.  That can't happen for INSERT but it is
		 * possible for UPDATE and DELETE.
		 */
		rte->checkForRead = false;
	}
	else
	{
		rte = refnameRangeTableEntry(pstate, relname);
		/*
		 * Since the rel was in the rangetable already, it's being read
		 * as well as written.  Therefore, leave checkForRead true.
		 *
		 * Force inh to the desired setting for the target (XXX is this
		 * reasonable?  It's *necessary* that INSERT target not be marked
		 * inheritable, but otherwise not too clear what to do if conflict?)
		 */
		rte->inh = inh;
	}

	/* Mark target table as requiring write access. */
	rte->checkForWrite = true;

	if (inJoinSet)
		addRTEtoJoinList(pstate, rte);

	/* lockTargetTable should have been called earlier */
	Assert(pstate->p_target_relation != NULL);

	pstate->p_target_rangetblentry = rte;
}


/*
 * Extract all not-in-common columns from column lists of a source table
 */
static void
extractUniqueColumns(List *common_colnames,
					 List *src_colnames, List *src_colvars,
					 List **res_colnames, List **res_colvars)
{
	List	   *new_colnames = NIL;
	List	   *new_colvars = NIL;
	List	   *lnames,
			   *lvars = src_colvars;

	foreach(lnames, src_colnames)
	{
		char	   *colname = strVal(lfirst(lnames));
		bool		match = false;
		List	   *cnames;

		foreach(cnames, common_colnames)
		{
			char	   *ccolname = strVal(lfirst(cnames));

			if (strcmp(colname, ccolname) == 0)
			{
				match = true;
				break;
			}
		}

		if (!match)
		{
			new_colnames = lappend(new_colnames, lfirst(lnames));
			new_colvars = lappend(new_colvars, lfirst(lvars));
		}

		lvars = lnext(lvars);
	}

	*res_colnames = new_colnames;
	*res_colvars = new_colvars;
}

/* transformJoinUsingClause()
 *	  Build a complete ON clause from a partially-transformed USING list.
 *	  We are given lists of nodes representing left and right match columns.
 *	  Result is a transformed qualification expression.
 */
static Node *
transformJoinUsingClause(ParseState *pstate, List *leftVars, List *rightVars)
{
	Node	   *result = NULL;
	List	   *lvars,
			   *rvars = rightVars;

	/*
	 * We cheat a little bit here by building an untransformed operator
	 * tree whose leaves are the already-transformed Vars.  This is OK
	 * because transformExpr() won't complain about already-transformed
	 * subnodes.
	 */
	foreach(lvars, leftVars)
	{
		Node	   *lvar = (Node *) lfirst(lvars);
		Node	   *rvar = (Node *) lfirst(rvars);
		A_Expr	   *e;

		e = makeNode(A_Expr);
		e->oper = OP;
		e->opname = "=";
		e->lexpr = copyObject(lvar);
		e->rexpr = copyObject(rvar);

		if (result == NULL)
			result = (Node *) e;
		else
		{
			A_Expr	   *a = makeNode(A_Expr);

			a->oper = AND;
			a->opname = NULL;
			a->lexpr = result;
			a->rexpr = (Node *) e;
			result = (Node *) a;
		}

		rvars = lnext(rvars);
	}

	/*
	 * Since the references are already Vars, and are certainly from the
	 * input relations, we don't have to go through the same pushups that
	 * transformJoinOnClause() does.  Just invoke transformExpr() to fix
	 * up the operators, and we're done.
	 */
	result = transformExpr(pstate, result, EXPR_COLUMN_FIRST);

	if (exprType(result) != BOOLOID)
	{
		/* This could only happen if someone defines a funny version of '=' */
		elog(ERROR, "JOIN/USING clause must return type bool, not type %s",
			 typeidTypeName(exprType(result)));
	}

	return result;
}	/* transformJoinUsingClause() */

/* transformJoinOnClause()
 *	  Transform the qual conditions for JOIN/ON.
 *	  Result is a transformed qualification expression.
 */
static Node *
transformJoinOnClause(ParseState *pstate, JoinExpr *j,
					  List *containedRels)
{
	Node	   *result;
	List	   *sv_joinlist;
	List	   *clause_varnos,
			   *l;

	/*
	 * This is a tad tricky, for two reasons.  First, at the point where
	 * we're called, the two subtrees of the JOIN node aren't yet part of
	 * the pstate's joinlist, which means that transformExpr() won't resolve
	 * unqualified references to their columns correctly.  We fix this in a
	 * slightly klugy way: temporarily make the pstate's joinlist consist of
	 * just those two subtrees (which creates exactly the namespace the ON
	 * clause should see).  This is OK only because the ON clause can't
	 * legally alter the joinlist by causing relation refs to be added.
	 */
	sv_joinlist = pstate->p_joinlist;
	pstate->p_joinlist = makeList2(j->larg, j->rarg);

	/* This part is just like transformWhereClause() */
	result = transformExpr(pstate, j->quals, EXPR_COLUMN_FIRST);
	if (exprType(result) != BOOLOID)
	{
		elog(ERROR, "JOIN/ON clause must return type bool, not type %s",
			 typeidTypeName(exprType(result)));
	}

	pstate->p_joinlist = sv_joinlist;

	/*
	 * Second, we need to check that the ON condition doesn't refer to any
	 * rels outside the input subtrees of the JOIN.  It could do that despite
	 * our hack on the joinlist if it uses fully-qualified names.  So, grovel
	 * through the transformed clause and make sure there are no bogus
	 * references.
	 */
	clause_varnos = pull_varnos(result);
	foreach(l, clause_varnos)
	{
		int		varno = lfirsti(l);

		if (! intMember(varno, containedRels))
		{
			elog(ERROR, "JOIN/ON clause refers to \"%s\", which is not part of JOIN",
				 rt_fetch(varno, pstate->p_rtable)->eref->relname);
		}
	}
	freeList(clause_varnos);

	return result;
}

/*
 * transformTableEntry --- transform a RangeVar (simple relation reference)
 */
static RangeTblRef *
transformTableEntry(ParseState *pstate, RangeVar *r)
{
	char	   *relname = r->relname;
	RangeTblEntry *rte;
	RangeTblRef *rtr;

	/*
	 * mark this entry to indicate it comes from the FROM clause. In SQL,
	 * the target list can only refer to range variables specified in the
	 * from clause but we follow the more powerful POSTQUEL semantics and
	 * automatically generate the range variable if not specified. However
	 * there are times we need to know whether the entries are legitimate.
	 */
	rte = addRangeTableEntry(pstate, relname, r->name, r->inh, true);

	/*
	 * We create a RangeTblRef, but we do not add it to the joinlist here.
	 * makeRangeTable will do so, if we are at top level of the FROM clause.
	 */
	rtr = makeNode(RangeTblRef);
	/* assume new rte is at end */
	rtr->rtindex = length(pstate->p_rtable);
	Assert(rte == rt_fetch(rtr->rtindex, pstate->p_rtable));

	return rtr;
}


/*
 * transformRangeSubselect --- transform a sub-SELECT appearing in FROM
 */
static RangeTblRef *
transformRangeSubselect(ParseState *pstate, RangeSubselect *r)
{
	List	   *save_rtable;
	List	   *save_joinlist;
	List	   *parsetrees;
	Query	   *query;
	RangeTblEntry *rte;
	RangeTblRef *rtr;

	/*
	 * We require user to supply an alias for a subselect, per SQL92.
	 * To relax this, we'd have to be prepared to gin up a unique alias
	 * for an unlabeled subselect.
	 */
	if (r->name == NULL)
		elog(ERROR, "sub-select in FROM must have an alias");

	/*
	 * Analyze and transform the subquery.  This is a bit tricky because
	 * we don't want the subquery to be able to see any FROM items already
	 * created in the current query (per SQL92, the scope of a FROM item
	 * does not include other FROM items).  But it does need to be able to
	 * see any further-up parent states, so we can't just pass a null parent
	 * pstate link.  So, temporarily make the current query level have an
	 * empty rtable and joinlist.
	 */
	save_rtable = pstate->p_rtable;
	save_joinlist = pstate->p_joinlist;
	pstate->p_rtable = NIL;
	pstate->p_joinlist = NIL;
	parsetrees = parse_analyze(r->subquery, pstate);
	pstate->p_rtable = save_rtable;
	pstate->p_joinlist = save_joinlist;

	/*
	 * Check that we got something reasonable.  Some of these conditions
	 * are probably impossible given restrictions of the grammar, but
	 * check 'em anyway.
	 */
	if (length(parsetrees) != 1)
		elog(ERROR, "Unexpected parse analysis result for subselect in FROM");
	query = (Query *) lfirst(parsetrees);
	if (query == NULL || !IsA(query, Query))
		elog(ERROR, "Unexpected parse analysis result for subselect in FROM");

	if (query->commandType != CMD_SELECT)
		elog(ERROR, "Expected SELECT query from subselect in FROM");
	if (query->resultRelation != 0 || query->into != NULL || query->isPortal)
		elog(ERROR, "Subselect in FROM may not have SELECT INTO");

	/*
	 * OK, build an RTE for the subquery.
	 */
	rte = addRangeTableEntryForSubquery(pstate, query, r->name, true);

	/*
	 * We create a RangeTblRef, but we do not add it to the joinlist here.
	 * makeRangeTable will do so, if we are at top level of the FROM clause.
	 */
	rtr = makeNode(RangeTblRef);
	/* assume new rte is at end */
	rtr->rtindex = length(pstate->p_rtable);
	Assert(rte == rt_fetch(rtr->rtindex, pstate->p_rtable));

	return rtr;
}


/*
 * transformFromClauseItem -
 *	  Transform a FROM-clause item, adding any required entries to the
 *	  range table list being built in the ParseState, and return the
 *	  transformed item ready to include in the joinlist.
 *	  This routine can recurse to handle SQL92 JOIN expressions.
 *
 *	  Aside from the primary return value (the transformed joinlist item)
 *	  this routine also returns an integer list of the rangetable indexes
 *	  of all the base relations represented in the joinlist item.  This
 *	  list is needed for checking JOIN/ON conditions in higher levels.
 */
static Node *
transformFromClauseItem(ParseState *pstate, Node *n, List **containedRels)
{
	if (IsA(n, RangeVar))
	{
		/* Plain relation reference */
		RangeTblRef *rtr;

		rtr = transformTableEntry(pstate, (RangeVar *) n);
		*containedRels = makeListi1(rtr->rtindex);
		return (Node *) rtr;
	}
	else if (IsA(n, RangeSubselect))
	{
		/* sub-SELECT is like a plain relation */
		RangeTblRef *rtr;

		rtr = transformRangeSubselect(pstate, (RangeSubselect *) n);
		*containedRels = makeListi1(rtr->rtindex);
		return (Node *) rtr;
	}
	else if (IsA(n, JoinExpr))
	{
		/* A newfangled join expression */
		JoinExpr   *j = (JoinExpr *) n;
		List	   *l_containedRels,
				   *r_containedRels,
				   *l_colnames,
				   *r_colnames,
				   *res_colnames,
				   *l_colvars,
				   *r_colvars,
				   *res_colvars;

		/*
		 * Recursively process the left and right subtrees
		 */
		j->larg = transformFromClauseItem(pstate, j->larg, &l_containedRels);
		j->rarg = transformFromClauseItem(pstate, j->rarg, &r_containedRels);

		/*
		 * Generate combined list of relation indexes
		 */
		*containedRels = nconc(l_containedRels, r_containedRels);

		/*
		 * Extract column name and var lists from both subtrees
		 */
		if (IsA(j->larg, JoinExpr))
		{
			/* Make a copy of the subtree's lists so we can modify! */
			l_colnames = copyObject(((JoinExpr *) j->larg)->colnames);
			l_colvars = copyObject(((JoinExpr *) j->larg)->colvars);
		}
		else
		{
			RangeTblEntry *rte;

			Assert(IsA(j->larg, RangeTblRef));
			rte = rt_fetch(((RangeTblRef *) j->larg)->rtindex,
						   pstate->p_rtable);
			expandRTE(pstate, rte, &l_colnames, &l_colvars);
			/* expandRTE returns new lists, so no need for copyObject */
		}
		if (IsA(j->rarg, JoinExpr))
		{
			/* Make a copy of the subtree's lists so we can modify! */
			r_colnames = copyObject(((JoinExpr *) j->rarg)->colnames);
			r_colvars = copyObject(((JoinExpr *) j->rarg)->colvars);
		}
		else
		{
			RangeTblEntry *rte;

			Assert(IsA(j->rarg, RangeTblRef));
			rte = rt_fetch(((RangeTblRef *) j->rarg)->rtindex,
						   pstate->p_rtable);
			expandRTE(pstate, rte, &r_colnames, &r_colvars);
			/* expandRTE returns new lists, so no need for copyObject */
		}

		/*
		 * Natural join does not explicitly specify columns; must
		 * generate columns to join. Need to run through the list of
		 * columns from each table or join result and match up the
		 * column names. Use the first table, and check every column
		 * in the second table for a match.  (We'll check that the
		 * matches were unique later on.)
		 * The result of this step is a list of column names just like an
		 * explicitly-written USING list.
		 */
		if (j->isNatural)
		{
			List	   *rlist = NIL;
			List	   *lx,
					   *rx;

			Assert(j->using == NIL); /* shouldn't have USING() too */

			foreach(lx, l_colnames)
			{
				char	   *l_colname = strVal(lfirst(lx));
				Value	   *m_name = NULL;

				foreach(rx, r_colnames)
				{
					char	   *r_colname = strVal(lfirst(rx));

					if (strcmp(l_colname, r_colname) == 0)
					{
						m_name = makeString(l_colname);
						break;
					}
				}

				/* matched a right column? then keep as join column... */
				if (m_name != NULL)
					rlist = lappend(rlist, m_name);
			}

			j->using = rlist;
		}

		/*
		 * Now transform the join qualifications, if any.
		 */
		res_colnames = NIL;
		res_colvars = NIL;

		if (j->using)
		{
			/*
			 * JOIN/USING (or NATURAL JOIN, as transformed above).
			 * Transform the list into an explicit ON-condition,
			 * and generate a list of result columns.
			 */
			List	   *ucols = j->using;
			List	   *l_usingvars = NIL;
			List	   *r_usingvars = NIL;
			List	   *ucol;

			Assert(j->quals == NULL); /* shouldn't have ON() too */

			foreach(ucol, ucols)
			{
				char	   *u_colname = strVal(lfirst(ucol));
				List	   *col;
				Node	   *l_colvar,
						   *r_colvar,
						   *colvar;
				int			ndx;
				int			l_index = -1;
				int			r_index = -1;

				ndx = 0;
				foreach(col, l_colnames)
				{
					char	   *l_colname = strVal(lfirst(col));

					if (strcmp(l_colname, u_colname) == 0)
					{
						if (l_index >= 0)
							elog(ERROR, "Common column name \"%s\" appears more than once in left table", u_colname);
						l_index = ndx;
					}
					ndx++;
				}
				if (l_index < 0)
					elog(ERROR, "JOIN/USING column \"%s\" not found in left table",
						 u_colname);

				ndx = 0;
				foreach(col, r_colnames)
				{
					char	   *r_colname = strVal(lfirst(col));

					if (strcmp(r_colname, u_colname) == 0)
					{
						if (r_index >= 0)
							elog(ERROR, "Common column name \"%s\" appears more than once in right table", u_colname);
						r_index = ndx;
					}
					ndx++;
				}
				if (r_index < 0)
					elog(ERROR, "JOIN/USING column \"%s\" not found in right table",
						 u_colname);

				l_colvar = nth(l_index, l_colvars);
				l_usingvars = lappend(l_usingvars, l_colvar);
				r_colvar = nth(r_index, r_colvars);
				r_usingvars = lappend(r_usingvars, r_colvar);

				res_colnames = lappend(res_colnames,
									   nth(l_index, l_colnames));
				switch (j->jointype)
				{
					case JOIN_INNER:
					case JOIN_LEFT:
						colvar = l_colvar;
						break;
					case JOIN_RIGHT:
						colvar = r_colvar;
						break;
					default:
					{
						/* Need COALESCE(l_colvar, r_colvar) */
						CaseExpr *c = makeNode(CaseExpr);
						CaseWhen *w = makeNode(CaseWhen);
						A_Expr *a = makeNode(A_Expr);

						a->oper = NOTNULL;
						a->lexpr = l_colvar;
						w->expr = (Node *) a;
						w->result = l_colvar;
						c->args = makeList1(w);
						c->defresult = r_colvar;
						colvar = transformExpr(pstate, (Node *) c,
											   EXPR_COLUMN_FIRST);
						break;
					}
				}
				res_colvars = lappend(res_colvars, colvar);
			}

			j->quals = transformJoinUsingClause(pstate,
												l_usingvars,
												r_usingvars);
		}
		else if (j->quals)
		{
			/* User-written ON-condition; transform it */
			j->quals = transformJoinOnClause(pstate, j, *containedRels);
		}
		else
		{
			/* CROSS JOIN: no quals */
		}

		/* Add remaining columns from each side to the output columns */
		extractUniqueColumns(res_colnames,
							 l_colnames, l_colvars,
							 &l_colnames, &l_colvars);
		extractUniqueColumns(res_colnames,
							 r_colnames, r_colvars,
							 &r_colnames, &r_colvars);
		res_colnames = nconc(res_colnames, l_colnames);
		res_colvars = nconc(res_colvars, l_colvars);
		res_colnames = nconc(res_colnames, r_colnames);
		res_colvars = nconc(res_colvars, r_colvars);

		/*
		 * Process alias (AS clause), if any.
		 *
		 * The given table alias must be unique in the current nesting level,
		 * ie it cannot match any RTE refname or jointable alias.  This is
		 * a bit painful to check because my own child joins are not yet in
		 * the pstate's joinlist, so they have to be scanned separately.
		 */
		if (j->alias)
		{
			/* Check against previously created RTEs and joinlist entries */
			if (refnameRangeOrJoinEntry(pstate, j->alias->relname, NULL))
				elog(ERROR, "Table name \"%s\" specified more than once",
					 j->alias->relname);
			/* Check children */
			if (scanJoinListForRefname(j->larg, j->alias->relname) ||
				scanJoinListForRefname(j->rarg, j->alias->relname))
				elog(ERROR, "Table name \"%s\" specified more than once",
					 j->alias->relname);
			/*
			 * If a column alias list is specified, substitute the alias
			 * names into my output-column list
			 */
			if (j->alias->attrs != NIL)
			{
				if (length(j->alias->attrs) != length(res_colnames))
					elog(ERROR, "Column alias list for \"%s\" has wrong number of entries (need %d)",
						 j->alias->relname, length(res_colnames));
				res_colnames = j->alias->attrs;
			}
		}

		j->colnames = res_colnames;
		j->colvars = res_colvars;

		return (Node *) j;
	}
	else
		elog(ERROR, "transformFromClauseItem: unexpected node (internal error)"
			 "\n\t%s", nodeToString(n));
	return NULL;				/* can't get here, just keep compiler quiet */
}


/*
 * transformWhereClause -
 *	  transforms the qualification and make sure it is of type Boolean
 */
Node *
transformWhereClause(ParseState *pstate, Node *clause)
{
	Node	   *qual;

	if (clause == NULL)
		return NULL;

	qual = transformExpr(pstate, clause, EXPR_COLUMN_FIRST);

	if (exprType(qual) != BOOLOID)
	{
		elog(ERROR, "WHERE clause must return type bool, not type %s",
			 typeidTypeName(exprType(qual)));
	}
	return qual;
}


/*
 *	findTargetlistEntry -
 *	  Returns the targetlist entry matching the given (untransformed) node.
 *	  If no matching entry exists, one is created and appended to the target
 *	  list as a "resjunk" node.
 *
 * node		the ORDER BY, GROUP BY, or DISTINCT ON expression to be matched
 * tlist	the existing target list (NB: this will never be NIL, which is a
 *			good thing since we'd be unable to append to it if it were...)
 * clause	identifies clause type being processed.
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
	 * 1. Bare ColumnName (no qualifier or subscripts)
	 *	  For a bare identifier, we search for a matching column name
	 *	  in the existing target list.	Multiple matches are an error
	 *	  unless they refer to identical values; for example,
	 *	  we allow	SELECT a, a FROM table ORDER BY a
	 *	  but not	SELECT a AS b, b FROM table ORDER BY b
	 *	  If no match is found, we fall through and treat the identifier
	 *	  as an expression.
	 *	  For GROUP BY, it is incorrect to match the grouping item against
	 *	  targetlist entries: according to SQL92, an identifier in GROUP BY
	 *	  is a reference to a column name exposed by FROM, not to a target
	 *	  list column.	However, many implementations (including pre-7.0
	 *	  PostgreSQL) accept this anyway.  So for GROUP BY, we look first
	 *	  to see if the identifier matches any FROM column name, and only
	 *	  try for a targetlist name if it doesn't.  This ensures that we
	 *	  adhere to the spec in the case where the name could be both.
	 *	  DISTINCT ON isn't in the standard, so we can do what we like there;
	 *	  we choose to make it work like ORDER BY, on the rather flimsy
	 *	  grounds that ordinary DISTINCT works on targetlist entries.
	 *
	 * 2. IntegerConstant
	 *	  This means to use the n'th item in the existing target list.
	 *	  Note that it would make no sense to order/group/distinct by an
	 *	  actual constant, so this does not create a conflict with our
	 *	  extension to order/group by an expression.
	 *	  GROUP BY column-number is not allowed by SQL92, but since
	 *	  the standard has no other behavior defined for this syntax,
	 *	  we may as well accept this common extension.
	 *
	 * Note that pre-existing resjunk targets must not be used in either case,
	 * since the user didn't write them in his SELECT list.
	 *
	 * If neither special case applies, fall through to treat the item as
	 * an expression.
	 *----------
	 */
	if (IsA(node, Ident) &&((Ident *) node)->indirection == NIL)
	{
		char	   *name = ((Ident *) node)->name;

		if (clause == GROUP_CLAUSE)
		{

			/*
			 * In GROUP BY, we must prefer a match against a FROM-clause
			 * column to one against the targetlist.  Look to see if there
			 * is a matching column.  If so, fall through to let
			 * transformExpr() do the rest.  NOTE: if name could refer
			 * ambiguously to more than one column name exposed by FROM,
			 * colnameToVar will elog(ERROR).  That's just what
			 * we want here.
			 */
			if (colnameToVar(pstate, name) != NULL)
				name = NULL;
		}

		if (name != NULL)
		{
			foreach(tl, tlist)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(tl);
				Resdom	   *resnode = tle->resdom;

				if (!resnode->resjunk &&
					strcmp(resnode->resname, name) == 0)
				{
					if (target_result != NULL)
					{
						if (!equal(target_result->expr, tle->expr))
							elog(ERROR, "%s '%s' is ambiguous",
								 clauseText[clause], name);
					}
					else
						target_result = tle;
					/* Stay in loop to check for ambiguity */
				}
			}
			if (target_result != NULL)
				return target_result;	/* return the first match */
		}
	}
	if (IsA(node, A_Const))
	{
		Value	   *val = &((A_Const *) node)->val;
		int			targetlist_pos = 0;
		int			target_pos;

		if (!IsA(val, Integer))
			elog(ERROR, "Non-integer constant in %s", clauseText[clause]);
		target_pos = intVal(val);
		foreach(tl, tlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(tl);
			Resdom	   *resnode = tle->resdom;

			if (!resnode->resjunk)
			{
				if (++targetlist_pos == target_pos)
					return tle; /* return the unique match */
			}
		}
		elog(ERROR, "%s position %d is not in target list",
			 clauseText[clause], target_pos);
	}

	/*
	 * Otherwise, we have an expression (this is a Postgres extension not
	 * found in SQL92).  Convert the untransformed node to a transformed
	 * expression, and search for a match in the tlist. NOTE: it doesn't
	 * really matter whether there is more than one match.	Also, we are
	 * willing to match a resjunk target here, though the above cases must
	 * ignore resjunk targets.
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
	 * the end of the target list.	This target is given resjunk = TRUE so
	 * that it will not be projected into the final tuple.
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
		if (!exprIsInSortList(tle->expr, glist, targetlist))
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
		SortGroupBy *sortby = lfirst(olitem);
		TargetEntry *tle;

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
 * is passed by reference.	We might also need to add items to the query's
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
		 * in the sort list should be added to it.	(We don't really care
		 * what order the DISTINCT fields are checked in, so we can leave
		 * the user's ORDER BY spec alone, and just add additional sort
		 * keys to it to ensure that all targetlist items get sorted.)
		 */
		*sortClause = addAllTargetsToSortList(*sortClause, targetlist);

		/*
		 * Now, DISTINCT list consists of all non-resjunk sortlist items.
		 * Actually, all the sortlist items had better be non-resjunk!
		 * Otherwise, user wrote SELECT DISTINCT with an ORDER BY item
		 * that does not appear anywhere in the SELECT targetlist, and we
		 * can't implement that with only one sorting pass...
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
		 * DISTINCT does, and we can't implement that with only one sort
		 * pass (and if we do two passes, the results will be rather
		 * unpredictable). However, it's OK to have more DISTINCT ON
		 * expressions than ORDER BY expressions; we can just add the
		 * extra DISTINCT values to the sort list, much as we did above
		 * for ordinary DISTINCT fields.
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

				/*
				 * Probably, the tle should always have been added at the
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

		if (!tle->resdom->resjunk)
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
	if (!exprIsInSortList(tle->expr, sortlist, targetlist))
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

	if (tle->resdom->ressortgroupref)	/* already has one? */
		return tle->resdom->ressortgroupref;

	/* easiest way to pick an unused refnumber: max used + 1 */
	maxRef = 0;
	foreach(l, tlist)
	{
		Index		ref = ((TargetEntry *) lfirst(l))->resdom->ressortgroupref;

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
