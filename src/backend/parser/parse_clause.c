/*-------------------------------------------------------------------------
 *
 * parse_clause.c
 *	  handle clauses in parser
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/parser/parse_clause.c,v 1.159 2006/11/28 12:54:41 petere Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/heap.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "rewrite/rewriteManip.h"
#include "utils/guc.h"


#define ORDER_CLAUSE 0
#define GROUP_CLAUSE 1
#define DISTINCT_ON_CLAUSE 2

static char *clauseText[] = {"ORDER BY", "GROUP BY", "DISTINCT ON"};

static void extractRemainingColumns(List *common_colnames,
						List *src_colnames, List *src_colvars,
						List **res_colnames, List **res_colvars);
static Node *transformJoinUsingClause(ParseState *pstate,
						 List *leftVars, List *rightVars);
static Node *transformJoinOnClause(ParseState *pstate, JoinExpr *j,
					  RangeTblEntry *l_rte,
					  RangeTblEntry *r_rte,
					  List *relnamespace,
					  Relids containedRels);
static RangeTblEntry *transformTableEntry(ParseState *pstate, RangeVar *r);
static RangeTblEntry *transformRangeSubselect(ParseState *pstate,
						RangeSubselect *r);
static RangeTblEntry *transformRangeFunction(ParseState *pstate,
					   RangeFunction *r);
static Node *transformFromClauseItem(ParseState *pstate, Node *n,
						RangeTblEntry **top_rte, int *top_rti,
						List **relnamespace,
						Relids *containedRels);
static Node *buildMergedJoinVar(ParseState *pstate, JoinType jointype,
				   Var *l_colvar, Var *r_colvar);
static TargetEntry *findTargetlistEntry(ParseState *pstate, Node *node,
					List **tlist, int clause);


/*
 * transformFromClause -
 *	  Process the FROM clause and add items to the query's range table,
 *	  joinlist, and namespaces.
 *
 * Note: we assume that pstate's p_rtable, p_joinlist, p_relnamespace, and
 * p_varnamespace lists were initialized to NIL when the pstate was created.
 * We will add onto any entries already present --- this is needed for rule
 * processing, as well as for UPDATE and DELETE.
 *
 * The range table may grow still further when we transform the expressions
 * in the query's quals and target list. (This is possible because in
 * POSTQUEL, we allowed references to relations not specified in the
 * from-clause.  PostgreSQL keeps this extension to standard SQL.)
 */
void
transformFromClause(ParseState *pstate, List *frmList)
{
	ListCell   *fl;

	/*
	 * The grammar will have produced a list of RangeVars, RangeSubselects,
	 * RangeFunctions, and/or JoinExprs. Transform each one (possibly adding
	 * entries to the rtable), check for duplicate refnames, and then add it
	 * to the joinlist and namespaces.
	 */
	foreach(fl, frmList)
	{
		Node	   *n = lfirst(fl);
		RangeTblEntry *rte;
		int			rtindex;
		List	   *relnamespace;
		Relids		containedRels;

		n = transformFromClauseItem(pstate, n,
									&rte,
									&rtindex,
									&relnamespace,
									&containedRels);
		checkNameSpaceConflicts(pstate, pstate->p_relnamespace, relnamespace);
		pstate->p_joinlist = lappend(pstate->p_joinlist, n);
		pstate->p_relnamespace = list_concat(pstate->p_relnamespace,
											 relnamespace);
		pstate->p_varnamespace = lappend(pstate->p_varnamespace, rte);
		bms_free(containedRels);
	}
}

/*
 * setTargetTable
 *	  Add the target relation of INSERT/UPDATE/DELETE to the range table,
 *	  and make the special links to it in the ParseState.
 *
 *	  We also open the target relation and acquire a write lock on it.
 *	  This must be done before processing the FROM list, in case the target
 *	  is also mentioned as a source relation --- we want to be sure to grab
 *	  the write lock before any read lock.
 *
 *	  If alsoSource is true, add the target to the query's joinlist and
 *	  namespace.  For INSERT, we don't want the target to be joined to;
 *	  it's a destination of tuples, not a source.	For UPDATE/DELETE,
 *	  we do need to scan or join the target.  (NOTE: we do not bother
 *	  to check for namespace conflict; we assume that the namespace was
 *	  initially empty in these cases.)
 *
 *	  Finally, we mark the relation as requiring the permissions specified
 *	  by requiredPerms.
 *
 *	  Returns the rangetable index of the target relation.
 */
int
setTargetTable(ParseState *pstate, RangeVar *relation,
			   bool inh, bool alsoSource, AclMode requiredPerms)
{
	RangeTblEntry *rte;
	int			rtindex;

	/* Close old target; this could only happen for multi-action rules */
	if (pstate->p_target_relation != NULL)
		heap_close(pstate->p_target_relation, NoLock);

	/*
	 * Open target rel and grab suitable lock (which we will hold till end of
	 * transaction).
	 *
	 * analyze.c will eventually do the corresponding heap_close(), but *not*
	 * release the lock.
	 */
	pstate->p_target_relation = heap_openrv(relation, RowExclusiveLock);

	/*
	 * Now build an RTE.
	 */
	rte = addRangeTableEntryForRelation(pstate, pstate->p_target_relation,
										relation->alias, inh, false);
	pstate->p_target_rangetblentry = rte;

	/* assume new rte is at end */
	rtindex = list_length(pstate->p_rtable);
	Assert(rte == rt_fetch(rtindex, pstate->p_rtable));

	/*
	 * Override addRangeTableEntry's default ACL_SELECT permissions check, and
	 * instead mark target table as requiring exactly the specified
	 * permissions.
	 *
	 * If we find an explicit reference to the rel later during parse
	 * analysis, we will add the ACL_SELECT bit back again; see
	 * scanRTEForColumn (for simple field references), ExpandColumnRefStar
	 * (for foo.*) and ExpandAllTables (for *).
	 */
	rte->requiredPerms = requiredPerms;

	/*
	 * If UPDATE/DELETE, add table to joinlist and namespaces.
	 */
	if (alsoSource)
		addRTEtoQuery(pstate, rte, true, true, true);

	return rtindex;
}

/*
 * Simplify InhOption (yes/no/default) into boolean yes/no.
 *
 * The reason we do things this way is that we don't want to examine the
 * SQL_inheritance option flag until parse_analyze is run.	Otherwise,
 * we'd do the wrong thing with query strings that intermix SET commands
 * with queries.
 */
bool
interpretInhOption(InhOption inhOpt)
{
	switch (inhOpt)
	{
		case INH_NO:
			return false;
		case INH_YES:
			return true;
		case INH_DEFAULT:
			return SQL_inheritance;
	}
	elog(ERROR, "bogus InhOption value: %d", inhOpt);
	return false;				/* keep compiler quiet */
}

/*
 * Given a relation-options list (of DefElems), return true iff the specified
 * table/result set should be created with OIDs. This needs to be done after
 * parsing the query string because the return value can depend upon the
 * default_with_oids GUC var.
 */
bool
interpretOidsOption(List *defList)
{
	ListCell   *cell;

	/* Scan list to see if OIDS was included */
	foreach(cell, defList)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (pg_strcasecmp(def->defname, "oids") == 0)
			return defGetBoolean(def);
	}

	/* OIDS option was not specified, so use default. */
	return default_with_oids;
}

/*
 * Extract all not-in-common columns from column lists of a source table
 */
static void
extractRemainingColumns(List *common_colnames,
						List *src_colnames, List *src_colvars,
						List **res_colnames, List **res_colvars)
{
	List	   *new_colnames = NIL;
	List	   *new_colvars = NIL;
	ListCell   *lnames,
			   *lvars;

	Assert(list_length(src_colnames) == list_length(src_colvars));

	forboth(lnames, src_colnames, lvars, src_colvars)
	{
		char	   *colname = strVal(lfirst(lnames));
		bool		match = false;
		ListCell   *cnames;

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
	ListCell   *lvars,
			   *rvars;

	/*
	 * We cheat a little bit here by building an untransformed operator tree
	 * whose leaves are the already-transformed Vars.  This is OK because
	 * transformExpr() won't complain about already-transformed subnodes.
	 */
	forboth(lvars, leftVars, rvars, rightVars)
	{
		Node	   *lvar = (Node *) lfirst(lvars);
		Node	   *rvar = (Node *) lfirst(rvars);
		A_Expr	   *e;

		e = makeSimpleA_Expr(AEXPR_OP, "=",
							 copyObject(lvar), copyObject(rvar),
							 -1);

		if (result == NULL)
			result = (Node *) e;
		else
		{
			A_Expr	   *a;

			a = makeA_Expr(AEXPR_AND, NIL, result, (Node *) e, -1);
			result = (Node *) a;
		}
	}

	/*
	 * Since the references are already Vars, and are certainly from the input
	 * relations, we don't have to go through the same pushups that
	 * transformJoinOnClause() does.  Just invoke transformExpr() to fix up
	 * the operators, and we're done.
	 */
	result = transformExpr(pstate, result);

	result = coerce_to_boolean(pstate, result, "JOIN/USING");

	return result;
}

/* transformJoinOnClause()
 *	  Transform the qual conditions for JOIN/ON.
 *	  Result is a transformed qualification expression.
 */
static Node *
transformJoinOnClause(ParseState *pstate, JoinExpr *j,
					  RangeTblEntry *l_rte,
					  RangeTblEntry *r_rte,
					  List *relnamespace,
					  Relids containedRels)
{
	Node	   *result;
	List	   *save_relnamespace;
	List	   *save_varnamespace;
	Relids		clause_varnos;
	int			varno;

	/*
	 * This is a tad tricky, for two reasons.  First, the namespace that the
	 * join expression should see is just the two subtrees of the JOIN plus
	 * any outer references from upper pstate levels.  So, temporarily set
	 * this pstate's namespace accordingly.  (We need not check for refname
	 * conflicts, because transformFromClauseItem() already did.) NOTE: this
	 * code is OK only because the ON clause can't legally alter the namespace
	 * by causing implicit relation refs to be added.
	 */
	save_relnamespace = pstate->p_relnamespace;
	save_varnamespace = pstate->p_varnamespace;

	pstate->p_relnamespace = relnamespace;
	pstate->p_varnamespace = list_make2(l_rte, r_rte);

	result = transformWhereClause(pstate, j->quals, "JOIN/ON");

	pstate->p_relnamespace = save_relnamespace;
	pstate->p_varnamespace = save_varnamespace;

	/*
	 * Second, we need to check that the ON condition doesn't refer to any
	 * rels outside the input subtrees of the JOIN.  It could do that despite
	 * our hack on the namespace if it uses fully-qualified names. So, grovel
	 * through the transformed clause and make sure there are no bogus
	 * references.	(Outer references are OK, and are ignored here.)
	 */
	clause_varnos = pull_varnos(result);
	clause_varnos = bms_del_members(clause_varnos, containedRels);
	if ((varno = bms_first_member(clause_varnos)) >= 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
		 errmsg("JOIN/ON clause refers to \"%s\", which is not part of JOIN",
				rt_fetch(varno, pstate->p_rtable)->eref->aliasname)));
	}
	bms_free(clause_varnos);

	return result;
}

/*
 * transformTableEntry --- transform a RangeVar (simple relation reference)
 */
static RangeTblEntry *
transformTableEntry(ParseState *pstate, RangeVar *r)
{
	RangeTblEntry *rte;

	/*
	 * mark this entry to indicate it comes from the FROM clause. In SQL, the
	 * target list can only refer to range variables specified in the from
	 * clause but we follow the more powerful POSTQUEL semantics and
	 * automatically generate the range variable if not specified. However
	 * there are times we need to know whether the entries are legitimate.
	 */
	rte = addRangeTableEntry(pstate, r, r->alias,
							 interpretInhOption(r->inhOpt), true);

	return rte;
}


/*
 * transformRangeSubselect --- transform a sub-SELECT appearing in FROM
 */
static RangeTblEntry *
transformRangeSubselect(ParseState *pstate, RangeSubselect *r)
{
	List	   *parsetrees;
	Query	   *query;
	RangeTblEntry *rte;

	/*
	 * We require user to supply an alias for a subselect, per SQL92. To relax
	 * this, we'd have to be prepared to gin up a unique alias for an
	 * unlabeled subselect.
	 */
	if (r->alias == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("subquery in FROM must have an alias")));

	/*
	 * Analyze and transform the subquery.
	 */
	parsetrees = parse_sub_analyze(r->subquery, pstate);

	/*
	 * Check that we got something reasonable.	Most of these conditions are
	 * probably impossible given restrictions of the grammar, but check 'em
	 * anyway.
	 */
	if (list_length(parsetrees) != 1)
		elog(ERROR, "unexpected parse analysis result for subquery in FROM");
	query = (Query *) linitial(parsetrees);
	if (query == NULL || !IsA(query, Query))
		elog(ERROR, "unexpected parse analysis result for subquery in FROM");

	if (query->commandType != CMD_SELECT)
		elog(ERROR, "expected SELECT query from subquery in FROM");
	if (query->into != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("subquery in FROM may not have SELECT INTO")));

	/*
	 * The subquery cannot make use of any variables from FROM items created
	 * earlier in the current query.  Per SQL92, the scope of a FROM item does
	 * not include other FROM items.  Formerly we hacked the namespace so that
	 * the other variables weren't even visible, but it seems more useful to
	 * leave them visible and give a specific error message.
	 *
	 * XXX this will need further work to support SQL99's LATERAL() feature,
	 * wherein such references would indeed be legal.
	 *
	 * We can skip groveling through the subquery if there's not anything
	 * visible in the current query.  Also note that outer references are OK.
	 */
	if (pstate->p_relnamespace || pstate->p_varnamespace)
	{
		if (contain_vars_of_level((Node *) query, 1))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 errmsg("subquery in FROM may not refer to other relations of same query level")));
	}

	/*
	 * OK, build an RTE for the subquery.
	 */
	rte = addRangeTableEntryForSubquery(pstate, query, r->alias, true);

	return rte;
}


/*
 * transformRangeFunction --- transform a function call appearing in FROM
 */
static RangeTblEntry *
transformRangeFunction(ParseState *pstate, RangeFunction *r)
{
	Node	   *funcexpr;
	char	   *funcname;
	RangeTblEntry *rte;

	/*
	 * Get function name for possible use as alias.  We use the same
	 * transformation rules as for a SELECT output expression.	For a FuncCall
	 * node, the result will be the function name, but it is possible for the
	 * grammar to hand back other node types.
	 */
	funcname = FigureColname(r->funccallnode);

	/*
	 * Transform the raw expression.
	 */
	funcexpr = transformExpr(pstate, r->funccallnode);

	/*
	 * The function parameters cannot make use of any variables from other
	 * FROM items.	(Compare to transformRangeSubselect(); the coding is
	 * different though because we didn't parse as a sub-select with its own
	 * level of namespace.)
	 *
	 * XXX this will need further work to support SQL99's LATERAL() feature,
	 * wherein such references would indeed be legal.
	 */
	if (pstate->p_relnamespace || pstate->p_varnamespace)
	{
		if (contain_vars_of_level(funcexpr, 0))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 errmsg("function expression in FROM may not refer to other relations of same query level")));
	}

	/*
	 * Disallow aggregate functions in the expression.	(No reason to postpone
	 * this check until parseCheckAggregates.)
	 */
	if (pstate->p_hasAggs)
	{
		if (checkExprHasAggs(funcexpr))
			ereport(ERROR,
					(errcode(ERRCODE_GROUPING_ERROR),
					 errmsg("cannot use aggregate function in function expression in FROM")));
	}

	/*
	 * OK, build an RTE for the function.
	 */
	rte = addRangeTableEntryForFunction(pstate, funcname, funcexpr,
										r, true);

	/*
	 * If a coldeflist was supplied, ensure it defines a legal set of names
	 * (no duplicates) and datatypes (no pseudo-types, for instance).
	 * addRangeTableEntryForFunction looked up the type names but didn't check
	 * them further than that.
	 */
	if (r->coldeflist)
	{
		TupleDesc	tupdesc;

		tupdesc = BuildDescFromLists(rte->eref->colnames,
									 rte->funccoltypes,
									 rte->funccoltypmods);
		CheckAttributeNamesTypes(tupdesc, RELKIND_COMPOSITE_TYPE);
	}

	return rte;
}


/*
 * transformFromClauseItem -
 *	  Transform a FROM-clause item, adding any required entries to the
 *	  range table list being built in the ParseState, and return the
 *	  transformed item ready to include in the joinlist and namespaces.
 *	  This routine can recurse to handle SQL92 JOIN expressions.
 *
 * The function return value is the node to add to the jointree (a
 * RangeTblRef or JoinExpr).  Additional output parameters are:
 *
 * *top_rte: receives the RTE corresponding to the jointree item.
 * (We could extract this from the function return node, but it saves cycles
 * to pass it back separately.)
 *
 * *top_rti: receives the rangetable index of top_rte.	(Ditto.)
 *
 * *relnamespace: receives a List of the RTEs exposed as relation names
 * by this item.
 *
 * *containedRels: receives a bitmap set of the rangetable indexes
 * of all the base and join relations represented in this jointree item.
 * This is needed for checking JOIN/ON conditions in higher levels.
 *
 * We do not need to pass back an explicit varnamespace value, because
 * in all cases the varnamespace contribution is exactly top_rte.
 */
static Node *
transformFromClauseItem(ParseState *pstate, Node *n,
						RangeTblEntry **top_rte, int *top_rti,
						List **relnamespace,
						Relids *containedRels)
{
	if (IsA(n, RangeVar))
	{
		/* Plain relation reference */
		RangeTblRef *rtr;
		RangeTblEntry *rte;
		int			rtindex;

		rte = transformTableEntry(pstate, (RangeVar *) n);
		/* assume new rte is at end */
		rtindex = list_length(pstate->p_rtable);
		Assert(rte == rt_fetch(rtindex, pstate->p_rtable));
		*top_rte = rte;
		*top_rti = rtindex;
		*relnamespace = list_make1(rte);
		*containedRels = bms_make_singleton(rtindex);
		rtr = makeNode(RangeTblRef);
		rtr->rtindex = rtindex;
		return (Node *) rtr;
	}
	else if (IsA(n, RangeSubselect))
	{
		/* sub-SELECT is like a plain relation */
		RangeTblRef *rtr;
		RangeTblEntry *rte;
		int			rtindex;

		rte = transformRangeSubselect(pstate, (RangeSubselect *) n);
		/* assume new rte is at end */
		rtindex = list_length(pstate->p_rtable);
		Assert(rte == rt_fetch(rtindex, pstate->p_rtable));
		*top_rte = rte;
		*top_rti = rtindex;
		*relnamespace = list_make1(rte);
		*containedRels = bms_make_singleton(rtindex);
		rtr = makeNode(RangeTblRef);
		rtr->rtindex = rtindex;
		return (Node *) rtr;
	}
	else if (IsA(n, RangeFunction))
	{
		/* function is like a plain relation */
		RangeTblRef *rtr;
		RangeTblEntry *rte;
		int			rtindex;

		rte = transformRangeFunction(pstate, (RangeFunction *) n);
		/* assume new rte is at end */
		rtindex = list_length(pstate->p_rtable);
		Assert(rte == rt_fetch(rtindex, pstate->p_rtable));
		*top_rte = rte;
		*top_rti = rtindex;
		*relnamespace = list_make1(rte);
		*containedRels = bms_make_singleton(rtindex);
		rtr = makeNode(RangeTblRef);
		rtr->rtindex = rtindex;
		return (Node *) rtr;
	}
	else if (IsA(n, JoinExpr))
	{
		/* A newfangled join expression */
		JoinExpr   *j = (JoinExpr *) n;
		RangeTblEntry *l_rte;
		RangeTblEntry *r_rte;
		int			l_rtindex;
		int			r_rtindex;
		Relids		l_containedRels,
					r_containedRels,
					my_containedRels;
		List	   *l_relnamespace,
				   *r_relnamespace,
				   *my_relnamespace,
				   *l_colnames,
				   *r_colnames,
				   *res_colnames,
				   *l_colvars,
				   *r_colvars,
				   *res_colvars;
		RangeTblEntry *rte;

		/*
		 * Recursively process the left and right subtrees
		 */
		j->larg = transformFromClauseItem(pstate, j->larg,
										  &l_rte,
										  &l_rtindex,
										  &l_relnamespace,
										  &l_containedRels);
		j->rarg = transformFromClauseItem(pstate, j->rarg,
										  &r_rte,
										  &r_rtindex,
										  &r_relnamespace,
										  &r_containedRels);

		/*
		 * Check for conflicting refnames in left and right subtrees. Must do
		 * this because higher levels will assume I hand back a self-
		 * consistent namespace subtree.
		 */
		checkNameSpaceConflicts(pstate, l_relnamespace, r_relnamespace);

		/*
		 * Generate combined relation membership info for possible use by
		 * transformJoinOnClause below.
		 */
		my_relnamespace = list_concat(l_relnamespace, r_relnamespace);
		my_containedRels = bms_join(l_containedRels, r_containedRels);

		pfree(r_relnamespace);	/* free unneeded list header */

		/*
		 * Extract column name and var lists from both subtrees
		 *
		 * Note: expandRTE returns new lists, safe for me to modify
		 */
		expandRTE(l_rte, l_rtindex, 0, false,
				  &l_colnames, &l_colvars);
		expandRTE(r_rte, r_rtindex, 0, false,
				  &r_colnames, &r_colvars);

		/*
		 * Natural join does not explicitly specify columns; must generate
		 * columns to join. Need to run through the list of columns from each
		 * table or join result and match up the column names. Use the first
		 * table, and check every column in the second table for a match.
		 * (We'll check that the matches were unique later on.) The result of
		 * this step is a list of column names just like an explicitly-written
		 * USING list.
		 */
		if (j->isNatural)
		{
			List	   *rlist = NIL;
			ListCell   *lx,
					   *rx;

			Assert(j->using == NIL);	/* shouldn't have USING() too */

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
			 * JOIN/USING (or NATURAL JOIN, as transformed above). Transform
			 * the list into an explicit ON-condition, and generate a list of
			 * merged result columns.
			 */
			List	   *ucols = j->using;
			List	   *l_usingvars = NIL;
			List	   *r_usingvars = NIL;
			ListCell   *ucol;

			Assert(j->quals == NULL);	/* shouldn't have ON() too */

			foreach(ucol, ucols)
			{
				char	   *u_colname = strVal(lfirst(ucol));
				ListCell   *col;
				int			ndx;
				int			l_index = -1;
				int			r_index = -1;
				Var		   *l_colvar,
						   *r_colvar;

				/* Check for USING(foo,foo) */
				foreach(col, res_colnames)
				{
					char	   *res_colname = strVal(lfirst(col));

					if (strcmp(res_colname, u_colname) == 0)
						ereport(ERROR,
								(errcode(ERRCODE_DUPLICATE_COLUMN),
								 errmsg("column name \"%s\" appears more than once in USING clause",
										u_colname)));
				}

				/* Find it in left input */
				ndx = 0;
				foreach(col, l_colnames)
				{
					char	   *l_colname = strVal(lfirst(col));

					if (strcmp(l_colname, u_colname) == 0)
					{
						if (l_index >= 0)
							ereport(ERROR,
									(errcode(ERRCODE_AMBIGUOUS_COLUMN),
									 errmsg("common column name \"%s\" appears more than once in left table",
											u_colname)));
						l_index = ndx;
					}
					ndx++;
				}
				if (l_index < 0)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" specified in USING clause does not exist in left table",
									u_colname)));

				/* Find it in right input */
				ndx = 0;
				foreach(col, r_colnames)
				{
					char	   *r_colname = strVal(lfirst(col));

					if (strcmp(r_colname, u_colname) == 0)
					{
						if (r_index >= 0)
							ereport(ERROR,
									(errcode(ERRCODE_AMBIGUOUS_COLUMN),
									 errmsg("common column name \"%s\" appears more than once in right table",
											u_colname)));
						r_index = ndx;
					}
					ndx++;
				}
				if (r_index < 0)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" specified in USING clause does not exist in right table",
									u_colname)));

				l_colvar = list_nth(l_colvars, l_index);
				l_usingvars = lappend(l_usingvars, l_colvar);
				r_colvar = list_nth(r_colvars, r_index);
				r_usingvars = lappend(r_usingvars, r_colvar);

				res_colnames = lappend(res_colnames, lfirst(ucol));
				res_colvars = lappend(res_colvars,
									  buildMergedJoinVar(pstate,
														 j->jointype,
														 l_colvar,
														 r_colvar));
			}

			j->quals = transformJoinUsingClause(pstate,
												l_usingvars,
												r_usingvars);
		}
		else if (j->quals)
		{
			/* User-written ON-condition; transform it */
			j->quals = transformJoinOnClause(pstate, j,
											 l_rte, r_rte,
											 my_relnamespace,
											 my_containedRels);
		}
		else
		{
			/* CROSS JOIN: no quals */
		}

		/* Add remaining columns from each side to the output columns */
		extractRemainingColumns(res_colnames,
								l_colnames, l_colvars,
								&l_colnames, &l_colvars);
		extractRemainingColumns(res_colnames,
								r_colnames, r_colvars,
								&r_colnames, &r_colvars);
		res_colnames = list_concat(res_colnames, l_colnames);
		res_colvars = list_concat(res_colvars, l_colvars);
		res_colnames = list_concat(res_colnames, r_colnames);
		res_colvars = list_concat(res_colvars, r_colvars);

		/*
		 * Check alias (AS clause), if any.
		 */
		if (j->alias)
		{
			if (j->alias->colnames != NIL)
			{
				if (list_length(j->alias->colnames) > list_length(res_colnames))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("column alias list for \"%s\" has too many entries",
									j->alias->aliasname)));
			}
		}

		/*
		 * Now build an RTE for the result of the join
		 */
		rte = addRangeTableEntryForJoin(pstate,
										res_colnames,
										j->jointype,
										res_colvars,
										j->alias,
										true);

		/* assume new rte is at end */
		j->rtindex = list_length(pstate->p_rtable);
		Assert(rte == rt_fetch(j->rtindex, pstate->p_rtable));

		*top_rte = rte;
		*top_rti = j->rtindex;

		/*
		 * Prepare returned namespace list.  If the JOIN has an alias then it
		 * hides the contained RTEs as far as the relnamespace goes;
		 * otherwise, put the contained RTEs and *not* the JOIN into
		 * relnamespace.
		 */
		if (j->alias)
		{
			*relnamespace = list_make1(rte);
			list_free(my_relnamespace);
		}
		else
			*relnamespace = my_relnamespace;

		/*
		 * Include join RTE in returned containedRels set
		 */
		*containedRels = bms_add_member(my_containedRels, j->rtindex);

		return (Node *) j;
	}
	else
		elog(ERROR, "unrecognized node type: %d", (int) nodeTag(n));
	return NULL;				/* can't get here, keep compiler quiet */
}

/*
 * buildMergedJoinVar -
 *	  generate a suitable replacement expression for a merged join column
 */
static Node *
buildMergedJoinVar(ParseState *pstate, JoinType jointype,
				   Var *l_colvar, Var *r_colvar)
{
	Oid			outcoltype;
	int32		outcoltypmod;
	Node	   *l_node,
			   *r_node,
			   *res_node;

	/*
	 * Choose output type if input types are dissimilar.
	 */
	outcoltype = l_colvar->vartype;
	outcoltypmod = l_colvar->vartypmod;
	if (outcoltype != r_colvar->vartype)
	{
		outcoltype = select_common_type(list_make2_oid(l_colvar->vartype,
													   r_colvar->vartype),
										"JOIN/USING");
		outcoltypmod = -1;		/* ie, unknown */
	}
	else if (outcoltypmod != r_colvar->vartypmod)
	{
		/* same type, but not same typmod */
		outcoltypmod = -1;		/* ie, unknown */
	}

	/*
	 * Insert coercion functions if needed.  Note that a difference in typmod
	 * can only happen if input has typmod but outcoltypmod is -1. In that
	 * case we insert a RelabelType to clearly mark that result's typmod is
	 * not same as input.  We never need coerce_type_typmod.
	 */
	if (l_colvar->vartype != outcoltype)
		l_node = coerce_type(pstate, (Node *) l_colvar, l_colvar->vartype,
							 outcoltype, outcoltypmod,
							 COERCION_IMPLICIT, COERCE_IMPLICIT_CAST);
	else if (l_colvar->vartypmod != outcoltypmod)
		l_node = (Node *) makeRelabelType((Expr *) l_colvar,
										  outcoltype, outcoltypmod,
										  COERCE_IMPLICIT_CAST);
	else
		l_node = (Node *) l_colvar;

	if (r_colvar->vartype != outcoltype)
		r_node = coerce_type(pstate, (Node *) r_colvar, r_colvar->vartype,
							 outcoltype, outcoltypmod,
							 COERCION_IMPLICIT, COERCE_IMPLICIT_CAST);
	else if (r_colvar->vartypmod != outcoltypmod)
		r_node = (Node *) makeRelabelType((Expr *) r_colvar,
										  outcoltype, outcoltypmod,
										  COERCE_IMPLICIT_CAST);
	else
		r_node = (Node *) r_colvar;

	/*
	 * Choose what to emit
	 */
	switch (jointype)
	{
		case JOIN_INNER:

			/*
			 * We can use either var; prefer non-coerced one if available.
			 */
			if (IsA(l_node, Var))
				res_node = l_node;
			else if (IsA(r_node, Var))
				res_node = r_node;
			else
				res_node = l_node;
			break;
		case JOIN_LEFT:
			/* Always use left var */
			res_node = l_node;
			break;
		case JOIN_RIGHT:
			/* Always use right var */
			res_node = r_node;
			break;
		case JOIN_FULL:
			{
				/*
				 * Here we must build a COALESCE expression to ensure that the
				 * join output is non-null if either input is.
				 */
				CoalesceExpr *c = makeNode(CoalesceExpr);

				c->coalescetype = outcoltype;
				c->args = list_make2(l_node, r_node);
				res_node = (Node *) c;
				break;
			}
		default:
			elog(ERROR, "unrecognized join type: %d", (int) jointype);
			res_node = NULL;	/* keep compiler quiet */
			break;
	}

	return res_node;
}


/*
 * transformWhereClause -
 *	  Transform the qualification and make sure it is of type boolean.
 *	  Used for WHERE and allied clauses.
 *
 * constructName does not affect the semantics, but is used in error messages
 */
Node *
transformWhereClause(ParseState *pstate, Node *clause,
					 const char *constructName)
{
	Node	   *qual;

	if (clause == NULL)
		return NULL;

	qual = transformExpr(pstate, clause);

	qual = coerce_to_boolean(pstate, qual, constructName);

	return qual;
}


/*
 * transformLimitClause -
 *	  Transform the expression and make sure it is of type bigint.
 *	  Used for LIMIT and allied clauses.
 *
 * Note: as of Postgres 8.2, LIMIT expressions are expected to yield int8,
 * rather than int4 as before.
 *
 * constructName does not affect the semantics, but is used in error messages
 */
Node *
transformLimitClause(ParseState *pstate, Node *clause,
					 const char *constructName)
{
	Node	   *qual;

	if (clause == NULL)
		return NULL;

	qual = transformExpr(pstate, clause);

	qual = coerce_to_bigint(pstate, qual, constructName);

	/*
	 * LIMIT can't refer to any vars or aggregates of the current query; we
	 * don't allow subselects either (though that case would at least be
	 * sensible)
	 */
	if (contain_vars_of_level(qual, 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
		/* translator: %s is name of a SQL construct, eg LIMIT */
				 errmsg("argument of %s must not contain variables",
						constructName)));
	}
	if (checkExprHasAggs(qual))
	{
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
		/* translator: %s is name of a SQL construct, eg LIMIT */
				 errmsg("argument of %s must not contain aggregates",
						constructName)));
	}
	if (contain_subplans(qual))
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/* translator: %s is name of a SQL construct, eg LIMIT */
				 errmsg("argument of %s must not contain subqueries",
						constructName)));
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
 * tlist	the target list (passed by reference so we can append to it)
 * clause	identifies clause type being processed
 */
static TargetEntry *
findTargetlistEntry(ParseState *pstate, Node *node, List **tlist, int clause)
{
	TargetEntry *target_result = NULL;
	ListCell   *tl;
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
	if (IsA(node, ColumnRef) &&
		list_length(((ColumnRef *) node)->fields) == 1)
	{
		char	   *name = strVal(linitial(((ColumnRef *) node)->fields));
		int			location = ((ColumnRef *) node)->location;

		if (clause == GROUP_CLAUSE)
		{
			/*
			 * In GROUP BY, we must prefer a match against a FROM-clause
			 * column to one against the targetlist.  Look to see if there is
			 * a matching column.  If so, fall through to let transformExpr()
			 * do the rest.  NOTE: if name could refer ambiguously to more
			 * than one column name exposed by FROM, colNameToVar will
			 * ereport(ERROR).	That's just what we want here.
			 *
			 * Small tweak for 7.4.3: ignore matches in upper query levels.
			 * This effectively changes the search order for bare names to (1)
			 * local FROM variables, (2) local targetlist aliases, (3) outer
			 * FROM variables, whereas before it was (1) (3) (2). SQL92 and
			 * SQL99 do not allow GROUPing BY an outer reference, so this
			 * breaks no cases that are legal per spec, and it seems a more
			 * self-consistent behavior.
			 */
			if (colNameToVar(pstate, name, true, location) != NULL)
				name = NULL;
		}

		if (name != NULL)
		{
			foreach(tl, *tlist)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(tl);

				if (!tle->resjunk &&
					strcmp(tle->resname, name) == 0)
				{
					if (target_result != NULL)
					{
						if (!equal(target_result->expr, tle->expr))
							ereport(ERROR,
									(errcode(ERRCODE_AMBIGUOUS_COLUMN),

							/*------
							  translator: first %s is name of a SQL construct, eg ORDER BY */
									 errmsg("%s \"%s\" is ambiguous",
											clauseText[clause], name),
									 parser_errposition(pstate, location)));
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
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
			/* translator: %s is name of a SQL construct, eg ORDER BY */
					 errmsg("non-integer constant in %s",
							clauseText[clause])));
		target_pos = intVal(val);
		foreach(tl, *tlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(tl);

			if (!tle->resjunk)
			{
				if (++targetlist_pos == target_pos)
					return tle; /* return the unique match */
			}
		}
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
		/* translator: %s is name of a SQL construct, eg ORDER BY */
				 errmsg("%s position %d is not in select list",
						clauseText[clause], target_pos)));
	}

	/*
	 * Otherwise, we have an expression (this is a Postgres extension not
	 * found in SQL92).  Convert the untransformed node to a transformed
	 * expression, and search for a match in the tlist. NOTE: it doesn't
	 * really matter whether there is more than one match.	Also, we are
	 * willing to match a resjunk target here, though the above cases must
	 * ignore resjunk targets.
	 */
	expr = transformExpr(pstate, node);

	foreach(tl, *tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tl);

		if (equal(expr, tle->expr))
			return tle;
	}

	/*
	 * If no matches, construct a new target entry which is appended to the
	 * end of the target list.	This target is given resjunk = TRUE so that it
	 * will not be projected into the final tuple.
	 */
	target_result = transformTargetEntry(pstate, node, expr, NULL, true);

	*tlist = lappend(*tlist, target_result);

	return target_result;
}

static GroupClause *
make_group_clause(TargetEntry *tle, List *targetlist, Oid sortop)
{
	GroupClause *result;

	result = makeNode(GroupClause);
	result->tleSortGroupRef = assignSortGroupRef(tle, targetlist);
	result->sortop = sortop;
	return result;
}

/*
 * transformGroupClause -
 *	  transform a GROUP BY clause
 *
 * GROUP BY items will be added to the targetlist (as resjunk columns)
 * if not already present, so the targetlist must be passed by reference.
 *
 * The order of the elements of the grouping clause does not affect
 * the semantics of the query. However, the optimizer is not currently
 * smart enough to reorder the grouping clause, so we try to do some
 * primitive reordering here.
 */
List *
transformGroupClause(ParseState *pstate, List *grouplist,
					 List **targetlist, List *sortClause)
{
	List	   *result = NIL;
	List	   *tle_list = NIL;
	ListCell   *l;

	/* Preprocess the grouping clause, lookup TLEs */
	foreach(l, grouplist)
	{
		TargetEntry *tle;
		Oid			restype;

		tle = findTargetlistEntry(pstate, lfirst(l),
								  targetlist, GROUP_CLAUSE);

		/* if tlist item is an UNKNOWN literal, change it to TEXT */
		restype = exprType((Node *) tle->expr);

		if (restype == UNKNOWNOID)
			tle->expr = (Expr *) coerce_type(pstate, (Node *) tle->expr,
											 restype, TEXTOID, -1,
											 COERCION_IMPLICIT,
											 COERCE_IMPLICIT_CAST);

		tle_list = lappend(tle_list, tle);
	}

	/*
	 * Now iterate through the ORDER BY clause. If we find a grouping element
	 * that matches the ORDER BY element, append the grouping element to the
	 * result set immediately. Otherwise, stop iterating. The effect of this
	 * is to look for a prefix of the ORDER BY list in the grouping clauses,
	 * and to move that prefix to the front of the GROUP BY.
	 */
	foreach(l, sortClause)
	{
		SortClause *sc = (SortClause *) lfirst(l);
		ListCell   *prev = NULL;
		ListCell   *tl;
		bool		found = false;

		foreach(tl, tle_list)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(tl);

			if (sc->tleSortGroupRef == tle->ressortgroupref)
			{
				GroupClause *gc;

				tle_list = list_delete_cell(tle_list, tl, prev);

				/* Use the sort clause's sorting operator */
				gc = make_group_clause(tle, *targetlist, sc->sortop);
				result = lappend(result, gc);
				found = true;
				break;
			}

			prev = tl;
		}

		/* As soon as we've failed to match an ORDER BY element, stop */
		if (!found)
			break;
	}

	/*
	 * Now add any remaining elements of the GROUP BY list in the order we
	 * received them.
	 *
	 * XXX: are there any additional criteria to consider when ordering
	 * grouping clauses?
	 */
	foreach(l, tle_list)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		GroupClause *gc;
		Oid			sort_op;

		/* avoid making duplicate grouplist entries */
		if (targetIsInSortList(tle, result))
			continue;

		sort_op = ordering_oper_opid(exprType((Node *) tle->expr));
		gc = make_group_clause(tle, *targetlist, sort_op);
		result = lappend(result, gc);
	}

	list_free(tle_list);
	return result;
}

/*
 * transformSortClause -
 *	  transform an ORDER BY clause
 *
 * ORDER BY items will be added to the targetlist (as resjunk columns)
 * if not already present, so the targetlist must be passed by reference.
 */
List *
transformSortClause(ParseState *pstate,
					List *orderlist,
					List **targetlist,
					bool resolveUnknown)
{
	List	   *sortlist = NIL;
	ListCell   *olitem;

	foreach(olitem, orderlist)
	{
		SortBy	   *sortby = lfirst(olitem);
		TargetEntry *tle;

		tle = findTargetlistEntry(pstate, sortby->node,
								  targetlist, ORDER_CLAUSE);

		sortlist = addTargetToSortList(pstate, tle,
									   sortlist, *targetlist,
									   sortby->sortby_kind,
									   sortby->useOp,
									   resolveUnknown);
	}

	return sortlist;
}

/*
 * transformDistinctClause -
 *	  transform a DISTINCT or DISTINCT ON clause
 *
 * Since we may need to add items to the query's sortClause list, that list
 * is passed by reference.	Likewise for the targetlist.
 */
List *
transformDistinctClause(ParseState *pstate, List *distinctlist,
						List **targetlist, List **sortClause)
{
	List	   *result = NIL;
	ListCell   *slitem;
	ListCell   *dlitem;

	/* No work if there was no DISTINCT clause */
	if (distinctlist == NIL)
		return NIL;

	if (linitial(distinctlist) == NULL)
	{
		/* We had SELECT DISTINCT */

		/*
		 * All non-resjunk elements from target list that are not already in
		 * the sort list should be added to it.  (We don't really care what
		 * order the DISTINCT fields are checked in, so we can leave the
		 * user's ORDER BY spec alone, and just add additional sort keys to it
		 * to ensure that all targetlist items get sorted.)
		 */
		*sortClause = addAllTargetsToSortList(pstate,
											  *sortClause,
											  *targetlist,
											  true);

		/*
		 * Now, DISTINCT list consists of all non-resjunk sortlist items.
		 * Actually, all the sortlist items had better be non-resjunk!
		 * Otherwise, user wrote SELECT DISTINCT with an ORDER BY item that
		 * does not appear anywhere in the SELECT targetlist, and we can't
		 * implement that with only one sorting pass...
		 */
		foreach(slitem, *sortClause)
		{
			SortClause *scl = (SortClause *) lfirst(slitem);
			TargetEntry *tle = get_sortgroupclause_tle(scl, *targetlist);

			if (tle->resjunk)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("for SELECT DISTINCT, ORDER BY expressions must appear in select list")));
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
		 * (and if we do two passes, the results will be rather
		 * unpredictable). However, it's OK to have more DISTINCT ON
		 * expressions than ORDER BY expressions; we can just add the extra
		 * DISTINCT values to the sort list, much as we did above for ordinary
		 * DISTINCT fields.
		 *
		 * Actually, it'd be OK for the common prefixes of the two lists to
		 * match in any order, but implementing that check seems like more
		 * trouble than it's worth.
		 */
		ListCell   *nextsortlist = list_head(*sortClause);

		foreach(dlitem, distinctlist)
		{
			TargetEntry *tle;

			tle = findTargetlistEntry(pstate, lfirst(dlitem),
									  targetlist, DISTINCT_ON_CLAUSE);

			if (nextsortlist != NULL)
			{
				SortClause *scl = (SortClause *) lfirst(nextsortlist);

				if (tle->ressortgroupref != scl->tleSortGroupRef)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
							 errmsg("SELECT DISTINCT ON expressions must match initial ORDER BY expressions")));
				result = lappend(result, copyObject(scl));
				nextsortlist = lnext(nextsortlist);
			}
			else
			{
				*sortClause = addTargetToSortList(pstate, tle,
												  *sortClause, *targetlist,
												  SORTBY_ASC, NIL, true);

				/*
				 * Probably, the tle should always have been added at the end
				 * of the sort list ... but search to be safe.
				 */
				foreach(slitem, *sortClause)
				{
					SortClause *scl = (SortClause *) lfirst(slitem);

					if (tle->ressortgroupref == scl->tleSortGroupRef)
					{
						result = lappend(result, copyObject(scl));
						break;
					}
				}
				if (slitem == NULL)		/* should not happen */
					elog(ERROR, "failed to add DISTINCT ON clause to target list");
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
 * See addTargetToSortList for info about pstate and resolveUnknown inputs.
 *
 * Returns the updated ORDER BY list.
 */
List *
addAllTargetsToSortList(ParseState *pstate, List *sortlist,
						List *targetlist, bool resolveUnknown)
{
	ListCell   *l;

	foreach(l, targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (!tle->resjunk)
			sortlist = addTargetToSortList(pstate, tle,
										   sortlist, targetlist,
										   SORTBY_ASC, NIL,
										   resolveUnknown);
	}
	return sortlist;
}

/*
 * addTargetToSortList
 *		If the given targetlist entry isn't already in the ORDER BY list,
 *		add it to the end of the list, using the sortop with given name
 *		or the default sort operator if opname == NIL.
 *
 * If resolveUnknown is TRUE, convert TLEs of type UNKNOWN to TEXT.  If not,
 * do nothing (which implies the search for a sort operator will fail).
 * pstate should be provided if resolveUnknown is TRUE, but can be NULL
 * otherwise.
 *
 * Returns the updated ORDER BY list.
 */
List *
addTargetToSortList(ParseState *pstate, TargetEntry *tle,
					List *sortlist, List *targetlist,
					int sortby_kind, List *sortby_opname,
					bool resolveUnknown)
{
	/* avoid making duplicate sortlist entries */
	if (!targetIsInSortList(tle, sortlist))
	{
		SortClause *sortcl = makeNode(SortClause);
		Oid			restype = exprType((Node *) tle->expr);

		/* if tlist item is an UNKNOWN literal, change it to TEXT */
		if (restype == UNKNOWNOID && resolveUnknown)
		{
			tle->expr = (Expr *) coerce_type(pstate, (Node *) tle->expr,
											 restype, TEXTOID, -1,
											 COERCION_IMPLICIT,
											 COERCE_IMPLICIT_CAST);
			restype = TEXTOID;
		}

		sortcl->tleSortGroupRef = assignSortGroupRef(tle, targetlist);

		switch (sortby_kind)
		{
			case SORTBY_ASC:
				sortcl->sortop = ordering_oper_opid(restype);
				break;
			case SORTBY_DESC:
				sortcl->sortop = reverse_ordering_oper_opid(restype);
				break;
			case SORTBY_USING:
				Assert(sortby_opname != NIL);
				sortcl->sortop = compatible_oper_opid(sortby_opname,
													  restype,
													  restype,
													  false);
				break;
			default:
				elog(ERROR, "unrecognized sortby_kind: %d", sortby_kind);
				break;
		}

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
	ListCell   *l;

	if (tle->ressortgroupref)	/* already has one? */
		return tle->ressortgroupref;

	/* easiest way to pick an unused refnumber: max used + 1 */
	maxRef = 0;
	foreach(l, tlist)
	{
		Index		ref = ((TargetEntry *) lfirst(l))->ressortgroupref;

		if (ref > maxRef)
			maxRef = ref;
	}
	tle->ressortgroupref = maxRef + 1;
	return tle->ressortgroupref;
}

/*
 * targetIsInSortList
 *		Is the given target item already in the sortlist?
 *
 * Works for both SortClause and GroupClause lists.  Note that the main
 * reason we need this routine (and not just a quick test for nonzeroness
 * of ressortgroupref) is that a TLE might be in only one of the lists.
 */
bool
targetIsInSortList(TargetEntry *tle, List *sortList)
{
	Index		ref = tle->ressortgroupref;
	ListCell   *l;

	/* no need to scan list if tle has no marker */
	if (ref == 0)
		return false;

	foreach(l, sortList)
	{
		SortClause *scl = (SortClause *) lfirst(l);

		if (scl->tleSortGroupRef == ref)
			return true;
	}
	return false;
}
