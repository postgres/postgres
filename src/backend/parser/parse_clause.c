/*-------------------------------------------------------------------------
 *
 * parse_clause.c
 *	  handle clauses in parser
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_clause.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"

#include "access/heapam.h"
#include "access/tsmapi.h"
#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "catalog/pg_am.h"
#include "catalog/pg_constraint_fn.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "parser/parser.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "rewrite/rewriteManip.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"


/* Convenience macro for the most common makeNamespaceItem() case */
#define makeDefaultNSItem(rte)	makeNamespaceItem(rte, true, true, false, true)

static void extractRemainingColumns(List *common_colnames,
						List *src_colnames, List *src_colvars,
						List **res_colnames, List **res_colvars);
static Node *transformJoinUsingClause(ParseState *pstate,
						 RangeTblEntry *leftRTE, RangeTblEntry *rightRTE,
						 List *leftVars, List *rightVars);
static Node *transformJoinOnClause(ParseState *pstate, JoinExpr *j,
					  List *namespace);
static RangeTblEntry *transformTableEntry(ParseState *pstate, RangeVar *r);
static RangeTblEntry *transformCTEReference(ParseState *pstate, RangeVar *r,
					  CommonTableExpr *cte, Index levelsup);
static RangeTblEntry *transformRangeSubselect(ParseState *pstate,
						RangeSubselect *r);
static RangeTblEntry *transformRangeFunction(ParseState *pstate,
					   RangeFunction *r);
static TableSampleClause *transformRangeTableSample(ParseState *pstate,
						  RangeTableSample *rts);
static Node *transformFromClauseItem(ParseState *pstate, Node *n,
						RangeTblEntry **top_rte, int *top_rti,
						List **namespace);
static Node *buildMergedJoinVar(ParseState *pstate, JoinType jointype,
				   Var *l_colvar, Var *r_colvar);
static ParseNamespaceItem *makeNamespaceItem(RangeTblEntry *rte,
				  bool rel_visible, bool cols_visible,
				  bool lateral_only, bool lateral_ok);
static void setNamespaceColumnVisibility(List *namespace, bool cols_visible);
static void setNamespaceLateralState(List *namespace,
						 bool lateral_only, bool lateral_ok);
static void checkExprIsVarFree(ParseState *pstate, Node *n,
				   const char *constructName);
static TargetEntry *findTargetlistEntrySQL92(ParseState *pstate, Node *node,
						 List **tlist, ParseExprKind exprKind);
static TargetEntry *findTargetlistEntrySQL99(ParseState *pstate, Node *node,
						 List **tlist, ParseExprKind exprKind);
static int get_matching_location(int sortgroupref,
					  List *sortgrouprefs, List *exprs);
static List *resolve_unique_index_expr(ParseState *pstate, InferClause *infer,
						  Relation heapRel);
static List *addTargetToGroupList(ParseState *pstate, TargetEntry *tle,
					 List *grouplist, List *targetlist, int location,
					 bool resolveUnknown);
static WindowClause *findWindowClause(List *wclist, const char *name);
static Node *transformFrameOffset(ParseState *pstate, int frameOptions,
					 Node *clause);


/*
 * transformFromClause -
 *	  Process the FROM clause and add items to the query's range table,
 *	  joinlist, and namespace.
 *
 * Note: we assume that the pstate's p_rtable, p_joinlist, and p_namespace
 * lists were initialized to NIL when the pstate was created.
 * We will add onto any entries already present --- this is needed for rule
 * processing, as well as for UPDATE and DELETE.
 */
void
transformFromClause(ParseState *pstate, List *frmList)
{
	ListCell   *fl;

	/*
	 * The grammar will have produced a list of RangeVars, RangeSubselects,
	 * RangeFunctions, and/or JoinExprs. Transform each one (possibly adding
	 * entries to the rtable), check for duplicate refnames, and then add it
	 * to the joinlist and namespace.
	 *
	 * Note we must process the items left-to-right for proper handling of
	 * LATERAL references.
	 */
	foreach(fl, frmList)
	{
		Node	   *n = lfirst(fl);
		RangeTblEntry *rte;
		int			rtindex;
		List	   *namespace;

		n = transformFromClauseItem(pstate, n,
									&rte,
									&rtindex,
									&namespace);

		checkNameSpaceConflicts(pstate, pstate->p_namespace, namespace);

		/* Mark the new namespace items as visible only to LATERAL */
		setNamespaceLateralState(namespace, true, true);

		pstate->p_joinlist = lappend(pstate->p_joinlist, n);
		pstate->p_namespace = list_concat(pstate->p_namespace, namespace);
	}

	/*
	 * We're done parsing the FROM list, so make all namespace items
	 * unconditionally visible.  Note that this will also reset lateral_only
	 * for any namespace items that were already present when we were called;
	 * but those should have been that way already.
	 */
	setNamespaceLateralState(pstate->p_namespace, false, true);
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
 *	  it's a destination of tuples, not a source.   For UPDATE/DELETE,
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
	 * free_parsestate() will eventually do the corresponding heap_close(),
	 * but *not* release the lock.
	 */
	pstate->p_target_relation = parserOpenTable(pstate, relation,
												RowExclusiveLock);

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
	 * markVarForSelectPriv and its callers.
	 */
	rte->requiredPerms = requiredPerms;

	/*
	 * If UPDATE/DELETE, add table to joinlist and namespace.
	 *
	 * Note: some callers know that they can find the new ParseNamespaceItem
	 * at the end of the pstate->p_namespace list.  This is a bit ugly but not
	 * worth complicating this function's signature for.
	 */
	if (alsoSource)
		addRTEtoQuery(pstate, rte, true, true, true);

	return rtindex;
}

/*
 * Simplify InhOption (yes/no/default) into boolean yes/no.
 *
 * The reason we do things this way is that we don't want to examine the
 * SQL_inheritance option flag until parse_analyze() is run.    Otherwise,
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
 *
 * In some situations, we want to reject an OIDS option even if it's present.
 * That's (rather messily) handled here rather than reloptions.c, because that
 * code explicitly punts checking for oids to here.
 */
bool
interpretOidsOption(List *defList, bool allowOids)
{
	ListCell   *cell;

	/* Scan list to see if OIDS was included */
	foreach(cell, defList)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (def->defnamespace == NULL &&
			pg_strcasecmp(def->defname, "oids") == 0)
		{
			if (!allowOids)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unrecognized parameter \"%s\"",
								def->defname)));
			return defGetBoolean(def);
		}
	}

	/* Force no-OIDS result if caller disallows OIDS. */
	if (!allowOids)
		return false;

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
transformJoinUsingClause(ParseState *pstate,
						 RangeTblEntry *leftRTE, RangeTblEntry *rightRTE,
						 List *leftVars, List *rightVars)
{
	Node	   *result;
	List	   *andargs = NIL;
	ListCell   *lvars,
			   *rvars;

	/*
	 * We cheat a little bit here by building an untransformed operator tree
	 * whose leaves are the already-transformed Vars.  This requires collusion
	 * from transformExpr(), which normally could be expected to complain
	 * about already-transformed subnodes.  However, this does mean that we
	 * have to mark the columns as requiring SELECT privilege for ourselves;
	 * transformExpr() won't do it.
	 */
	forboth(lvars, leftVars, rvars, rightVars)
	{
		Var		   *lvar = (Var *) lfirst(lvars);
		Var		   *rvar = (Var *) lfirst(rvars);
		A_Expr	   *e;

		/* Require read access to the join variables */
		markVarForSelectPriv(pstate, lvar, leftRTE);
		markVarForSelectPriv(pstate, rvar, rightRTE);

		/* Now create the lvar = rvar join condition */
		e = makeSimpleA_Expr(AEXPR_OP, "=",
							 copyObject(lvar), copyObject(rvar),
							 -1);

		/* Prepare to combine into an AND clause, if multiple join columns */
		andargs = lappend(andargs, e);
	}

	/* Only need an AND if there's more than one join column */
	if (list_length(andargs) == 1)
		result = (Node *) linitial(andargs);
	else
		result = (Node *) makeBoolExpr(AND_EXPR, andargs, -1);

	/*
	 * Since the references are already Vars, and are certainly from the input
	 * relations, we don't have to go through the same pushups that
	 * transformJoinOnClause() does.  Just invoke transformExpr() to fix up
	 * the operators, and we're done.
	 */
	result = transformExpr(pstate, result, EXPR_KIND_JOIN_USING);

	result = coerce_to_boolean(pstate, result, "JOIN/USING");

	return result;
}

/* transformJoinOnClause()
 *	  Transform the qual conditions for JOIN/ON.
 *	  Result is a transformed qualification expression.
 */
static Node *
transformJoinOnClause(ParseState *pstate, JoinExpr *j, List *namespace)
{
	Node	   *result;
	List	   *save_namespace;

	/*
	 * The namespace that the join expression should see is just the two
	 * subtrees of the JOIN plus any outer references from upper pstate
	 * levels.  Temporarily set this pstate's namespace accordingly.  (We need
	 * not check for refname conflicts, because transformFromClauseItem()
	 * already did.)  All namespace items are marked visible regardless of
	 * LATERAL state.
	 */
	setNamespaceLateralState(namespace, false, true);

	save_namespace = pstate->p_namespace;
	pstate->p_namespace = namespace;

	result = transformWhereClause(pstate, j->quals,
								  EXPR_KIND_JOIN_ON, "JOIN/ON");

	pstate->p_namespace = save_namespace;

	return result;
}

/*
 * transformTableEntry --- transform a RangeVar (simple relation reference)
 */
static RangeTblEntry *
transformTableEntry(ParseState *pstate, RangeVar *r)
{
	RangeTblEntry *rte;

	/* We need only build a range table entry */
	rte = addRangeTableEntry(pstate, r, r->alias,
							 interpretInhOption(r->inhOpt), true);

	return rte;
}

/*
 * transformCTEReference --- transform a RangeVar that references a common
 * table expression (ie, a sub-SELECT defined in a WITH clause)
 */
static RangeTblEntry *
transformCTEReference(ParseState *pstate, RangeVar *r,
					  CommonTableExpr *cte, Index levelsup)
{
	RangeTblEntry *rte;

	rte = addRangeTableEntryForCTE(pstate, cte, levelsup, r, true);

	return rte;
}

/*
 * transformRangeSubselect --- transform a sub-SELECT appearing in FROM
 */
static RangeTblEntry *
transformRangeSubselect(ParseState *pstate, RangeSubselect *r)
{
	Query	   *query;
	RangeTblEntry *rte;

	/*
	 * We require user to supply an alias for a subselect, per SQL92. To relax
	 * this, we'd have to be prepared to gin up a unique alias for an
	 * unlabeled subselect.  (This is just elog, not ereport, because the
	 * grammar should have enforced it already.  It'd probably be better to
	 * report the error here, but we don't have a good error location here.)
	 */
	if (r->alias == NULL)
		elog(ERROR, "subquery in FROM must have an alias");

	/*
	 * Set p_expr_kind to show this parse level is recursing to a subselect.
	 * We can't be nested within any expression, so don't need save-restore
	 * logic here.
	 */
	Assert(pstate->p_expr_kind == EXPR_KIND_NONE);
	pstate->p_expr_kind = EXPR_KIND_FROM_SUBSELECT;

	/*
	 * If the subselect is LATERAL, make lateral_only names of this level
	 * visible to it.  (LATERAL can't nest within a single pstate level, so we
	 * don't need save/restore logic here.)
	 */
	Assert(!pstate->p_lateral_active);
	pstate->p_lateral_active = r->lateral;

	/*
	 * Analyze and transform the subquery.
	 */
	query = parse_sub_analyze(r->subquery, pstate, NULL,
							  isLockedRefname(pstate, r->alias->aliasname));

	/* Restore state */
	pstate->p_lateral_active = false;
	pstate->p_expr_kind = EXPR_KIND_NONE;

	/*
	 * Check that we got something reasonable.  Many of these conditions are
	 * impossible given restrictions of the grammar, but check 'em anyway.
	 */
	if (!IsA(query, Query) ||
		query->commandType != CMD_SELECT ||
		query->utilityStmt != NULL)
		elog(ERROR, "unexpected non-SELECT command in subquery in FROM");

	/*
	 * OK, build an RTE for the subquery.
	 */
	rte = addRangeTableEntryForSubquery(pstate,
										query,
										r->alias,
										r->lateral,
										true);

	return rte;
}


/*
 * transformRangeFunction --- transform a function call appearing in FROM
 */
static RangeTblEntry *
transformRangeFunction(ParseState *pstate, RangeFunction *r)
{
	List	   *funcexprs = NIL;
	List	   *funcnames = NIL;
	List	   *coldeflists = NIL;
	bool		is_lateral;
	RangeTblEntry *rte;
	ListCell   *lc;

	/*
	 * We make lateral_only names of this level visible, whether or not the
	 * RangeFunction is explicitly marked LATERAL.  This is needed for SQL
	 * spec compliance in the case of UNNEST(), and seems useful on
	 * convenience grounds for all functions in FROM.
	 *
	 * (LATERAL can't nest within a single pstate level, so we don't need
	 * save/restore logic here.)
	 */
	Assert(!pstate->p_lateral_active);
	pstate->p_lateral_active = true;

	/*
	 * Transform the raw expressions.
	 *
	 * While transforming, also save function names for possible use as alias
	 * and column names.  We use the same transformation rules as for a SELECT
	 * output expression.  For a FuncCall node, the result will be the
	 * function name, but it is possible for the grammar to hand back other
	 * node types.
	 *
	 * We have to get this info now, because FigureColname only works on raw
	 * parsetrees.  Actually deciding what to do with the names is left up to
	 * addRangeTableEntryForFunction.
	 *
	 * Likewise, collect column definition lists if there were any.  But
	 * complain if we find one here and the RangeFunction has one too.
	 */
	foreach(lc, r->functions)
	{
		List	   *pair = (List *) lfirst(lc);
		Node	   *fexpr;
		List	   *coldeflist;

		/* Disassemble the function-call/column-def-list pairs */
		Assert(list_length(pair) == 2);
		fexpr = (Node *) linitial(pair);
		coldeflist = (List *) lsecond(pair);

		/*
		 * If we find a function call unnest() with more than one argument and
		 * no special decoration, transform it into separate unnest() calls on
		 * each argument.  This is a kluge, for sure, but it's less nasty than
		 * other ways of implementing the SQL-standard UNNEST() syntax.
		 *
		 * If there is any decoration (including a coldeflist), we don't
		 * transform, which probably means a no-such-function error later.  We
		 * could alternatively throw an error right now, but that doesn't seem
		 * tremendously helpful.  If someone is using any such decoration,
		 * then they're not using the SQL-standard syntax, and they're more
		 * likely expecting an un-tweaked function call.
		 *
		 * Note: the transformation changes a non-schema-qualified unnest()
		 * function name into schema-qualified pg_catalog.unnest().  This
		 * choice is also a bit debatable, but it seems reasonable to force
		 * use of built-in unnest() when we make this transformation.
		 */
		if (IsA(fexpr, FuncCall))
		{
			FuncCall   *fc = (FuncCall *) fexpr;

			if (list_length(fc->funcname) == 1 &&
				strcmp(strVal(linitial(fc->funcname)), "unnest") == 0 &&
				list_length(fc->args) > 1 &&
				fc->agg_order == NIL &&
				fc->agg_filter == NULL &&
				!fc->agg_star &&
				!fc->agg_distinct &&
				!fc->func_variadic &&
				fc->over == NULL &&
				coldeflist == NIL)
			{
				ListCell   *lc;

				foreach(lc, fc->args)
				{
					Node	   *arg = (Node *) lfirst(lc);
					FuncCall   *newfc;

					newfc = makeFuncCall(SystemFuncName("unnest"),
										 list_make1(arg),
										 fc->location);

					funcexprs = lappend(funcexprs,
										transformExpr(pstate, (Node *) newfc,
												   EXPR_KIND_FROM_FUNCTION));

					funcnames = lappend(funcnames,
										FigureColname((Node *) newfc));

					/* coldeflist is empty, so no error is possible */

					coldeflists = lappend(coldeflists, coldeflist);
				}
				continue;		/* done with this function item */
			}
		}

		/* normal case ... */
		funcexprs = lappend(funcexprs,
							transformExpr(pstate, fexpr,
										  EXPR_KIND_FROM_FUNCTION));

		funcnames = lappend(funcnames,
							FigureColname(fexpr));

		if (coldeflist && r->coldeflist)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("multiple column definition lists are not allowed for the same function"),
					 parser_errposition(pstate,
									 exprLocation((Node *) r->coldeflist))));

		coldeflists = lappend(coldeflists, coldeflist);
	}

	pstate->p_lateral_active = false;

	/*
	 * We must assign collations now so that the RTE exposes correct collation
	 * info for Vars created from it.
	 */
	assign_list_collations(pstate, funcexprs);

	/*
	 * Install the top-level coldeflist if there was one (we already checked
	 * that there was no conflicting per-function coldeflist).
	 *
	 * We only allow this when there's a single function (even after UNNEST
	 * expansion) and no WITH ORDINALITY.  The reason for the latter
	 * restriction is that it's not real clear whether the ordinality column
	 * should be in the coldeflist, and users are too likely to make mistakes
	 * in one direction or the other.  Putting the coldeflist inside ROWS
	 * FROM() is much clearer in this case.
	 */
	if (r->coldeflist)
	{
		if (list_length(funcexprs) != 1)
		{
			if (r->is_rowsfrom)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("ROWS FROM() with multiple functions cannot have a column definition list"),
						 errhint("Put a separate column definition list for each function inside ROWS FROM()."),
						 parser_errposition(pstate,
									 exprLocation((Node *) r->coldeflist))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("UNNEST() with multiple arguments cannot have a column definition list"),
						 errhint("Use separate UNNEST() calls inside ROWS FROM(), and attach a column definition list to each one."),
						 parser_errposition(pstate,
									 exprLocation((Node *) r->coldeflist))));
		}
		if (r->ordinality)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("WITH ORDINALITY cannot be used with a column definition list"),
			   errhint("Put the column definition list inside ROWS FROM()."),
					 parser_errposition(pstate,
									 exprLocation((Node *) r->coldeflist))));

		coldeflists = list_make1(r->coldeflist);
	}

	/*
	 * Mark the RTE as LATERAL if the user said LATERAL explicitly, or if
	 * there are any lateral cross-references in it.
	 */
	is_lateral = r->lateral || contain_vars_of_level((Node *) funcexprs, 0);

	/*
	 * OK, build an RTE for the function.
	 */
	rte = addRangeTableEntryForFunction(pstate,
										funcnames, funcexprs, coldeflists,
										r, is_lateral, true);

	return rte;
}

/*
 * transformRangeTableSample --- transform a TABLESAMPLE clause
 *
 * Caller has already transformed rts->relation, we just have to validate
 * the remaining fields and create a TableSampleClause node.
 */
static TableSampleClause *
transformRangeTableSample(ParseState *pstate, RangeTableSample *rts)
{
	TableSampleClause *tablesample;
	Oid			handlerOid;
	Oid			funcargtypes[1];
	TsmRoutine *tsm;
	List	   *fargs;
	ListCell   *larg,
			   *ltyp;

	/*
	 * To validate the sample method name, look up the handler function, which
	 * has the same name, one dummy INTERNAL argument, and a result type of
	 * tsm_handler.  (Note: tablesample method names are not schema-qualified
	 * in the SQL standard; but since they are just functions to us, we allow
	 * schema qualification to resolve any potential ambiguity.)
	 */
	funcargtypes[0] = INTERNALOID;

	handlerOid = LookupFuncName(rts->method, 1, funcargtypes, true);

	/* we want error to complain about no-such-method, not no-such-function */
	if (!OidIsValid(handlerOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablesample method %s does not exist",
						NameListToString(rts->method)),
				 parser_errposition(pstate, rts->location)));

	/* check that handler has correct return type */
	if (get_func_rettype(handlerOid) != TSM_HANDLEROID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("function %s must return type %s",
						NameListToString(rts->method), "tsm_handler"),
				 parser_errposition(pstate, rts->location)));

	/* OK, run the handler to get TsmRoutine, for argument type info */
	tsm = GetTsmRoutine(handlerOid);

	tablesample = makeNode(TableSampleClause);
	tablesample->tsmhandler = handlerOid;

	/* check user provided the expected number of arguments */
	if (list_length(rts->args) != list_length(tsm->parameterTypes))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLESAMPLE_ARGUMENT),
		  errmsg_plural("tablesample method %s requires %d argument, not %d",
						"tablesample method %s requires %d arguments, not %d",
						list_length(tsm->parameterTypes),
						NameListToString(rts->method),
						list_length(tsm->parameterTypes),
						list_length(rts->args)),
				 parser_errposition(pstate, rts->location)));

	/*
	 * Transform the arguments, typecasting them as needed.  Note we must also
	 * assign collations now, because assign_query_collations() doesn't
	 * examine any substructure of RTEs.
	 */
	fargs = NIL;
	forboth(larg, rts->args, ltyp, tsm->parameterTypes)
	{
		Node	   *arg = (Node *) lfirst(larg);
		Oid			argtype = lfirst_oid(ltyp);

		arg = transformExpr(pstate, arg, EXPR_KIND_FROM_FUNCTION);
		arg = coerce_to_specific_type(pstate, arg, argtype, "TABLESAMPLE");
		assign_expr_collations(pstate, arg);
		fargs = lappend(fargs, arg);
	}
	tablesample->args = fargs;

	/* Process REPEATABLE (seed) */
	if (rts->repeatable != NULL)
	{
		Node	   *arg;

		if (!tsm->repeatable_across_queries)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				  errmsg("tablesample method %s does not support REPEATABLE",
						 NameListToString(rts->method)),
					 parser_errposition(pstate, rts->location)));

		arg = transformExpr(pstate, rts->repeatable, EXPR_KIND_FROM_FUNCTION);
		arg = coerce_to_specific_type(pstate, arg, FLOAT8OID, "REPEATABLE");
		assign_expr_collations(pstate, arg);
		tablesample->repeatable = (Expr *) arg;
	}
	else
		tablesample->repeatable = NULL;

	return tablesample;
}


/*
 * transformFromClauseItem -
 *	  Transform a FROM-clause item, adding any required entries to the
 *	  range table list being built in the ParseState, and return the
 *	  transformed item ready to include in the joinlist.  Also build a
 *	  ParseNamespaceItem list describing the names exposed by this item.
 *	  This routine can recurse to handle SQL92 JOIN expressions.
 *
 * The function return value is the node to add to the jointree (a
 * RangeTblRef or JoinExpr).  Additional output parameters are:
 *
 * *top_rte: receives the RTE corresponding to the jointree item.
 * (We could extract this from the function return node, but it saves cycles
 * to pass it back separately.)
 *
 * *top_rti: receives the rangetable index of top_rte.  (Ditto.)
 *
 * *namespace: receives a List of ParseNamespaceItems for the RTEs exposed
 * as table/column names by this item.  (The lateral_only flags in these items
 * are indeterminate and should be explicitly set by the caller before use.)
 */
static Node *
transformFromClauseItem(ParseState *pstate, Node *n,
						RangeTblEntry **top_rte, int *top_rti,
						List **namespace)
{
	if (IsA(n, RangeVar))
	{
		/* Plain relation reference, or perhaps a CTE reference */
		RangeVar   *rv = (RangeVar *) n;
		RangeTblRef *rtr;
		RangeTblEntry *rte = NULL;
		int			rtindex;

		/* if it is an unqualified name, it might be a CTE reference */
		if (!rv->schemaname)
		{
			CommonTableExpr *cte;
			Index		levelsup;

			cte = scanNameSpaceForCTE(pstate, rv->relname, &levelsup);
			if (cte)
				rte = transformCTEReference(pstate, rv, cte, levelsup);
		}

		/* if not found as a CTE, must be a table reference */
		if (!rte)
			rte = transformTableEntry(pstate, rv);

		/* assume new rte is at end */
		rtindex = list_length(pstate->p_rtable);
		Assert(rte == rt_fetch(rtindex, pstate->p_rtable));
		*top_rte = rte;
		*top_rti = rtindex;
		*namespace = list_make1(makeDefaultNSItem(rte));
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
		*namespace = list_make1(makeDefaultNSItem(rte));
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
		*namespace = list_make1(makeDefaultNSItem(rte));
		rtr = makeNode(RangeTblRef);
		rtr->rtindex = rtindex;
		return (Node *) rtr;
	}
	else if (IsA(n, RangeTableSample))
	{
		/* TABLESAMPLE clause (wrapping some other valid FROM node) */
		RangeTableSample *rts = (RangeTableSample *) n;
		Node	   *rel;
		RangeTblRef *rtr;
		RangeTblEntry *rte;

		/* Recursively transform the contained relation */
		rel = transformFromClauseItem(pstate, rts->relation,
									  top_rte, top_rti, namespace);
		/* Currently, grammar could only return a RangeVar as contained rel */
		Assert(IsA(rel, RangeTblRef));
		rtr = (RangeTblRef *) rel;
		rte = rt_fetch(rtr->rtindex, pstate->p_rtable);
		/* We only support this on plain relations and matviews */
		if (rte->relkind != RELKIND_RELATION &&
			rte->relkind != RELKIND_MATVIEW)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("TABLESAMPLE clause can only be applied to tables and materialized views"),
				   parser_errposition(pstate, exprLocation(rts->relation))));

		/* Transform TABLESAMPLE details and attach to the RTE */
		rte->tablesample = transformRangeTableSample(pstate, rts);
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
		List	   *l_namespace,
				   *r_namespace,
				   *my_namespace,
				   *l_colnames,
				   *r_colnames,
				   *res_colnames,
				   *l_colvars,
				   *r_colvars,
				   *res_colvars;
		bool		lateral_ok;
		int			sv_namespace_length;
		RangeTblEntry *rte;
		int			k;

		/*
		 * Recursively process the left subtree, then the right.  We must do
		 * it in this order for correct visibility of LATERAL references.
		 */
		j->larg = transformFromClauseItem(pstate, j->larg,
										  &l_rte,
										  &l_rtindex,
										  &l_namespace);

		/*
		 * Make the left-side RTEs available for LATERAL access within the
		 * right side, by temporarily adding them to the pstate's namespace
		 * list.  Per SQL:2008, if the join type is not INNER or LEFT then the
		 * left-side names must still be exposed, but it's an error to
		 * reference them.  (Stupid design, but that's what it says.)  Hence,
		 * we always push them into the namespace, but mark them as not
		 * lateral_ok if the jointype is wrong.
		 *
		 * Notice that we don't require the merged namespace list to be
		 * conflict-free.  See the comments for scanNameSpaceForRefname().
		 *
		 * NB: this coding relies on the fact that list_concat is not
		 * destructive to its second argument.
		 */
		lateral_ok = (j->jointype == JOIN_INNER || j->jointype == JOIN_LEFT);
		setNamespaceLateralState(l_namespace, true, lateral_ok);

		sv_namespace_length = list_length(pstate->p_namespace);
		pstate->p_namespace = list_concat(pstate->p_namespace, l_namespace);

		/* And now we can process the RHS */
		j->rarg = transformFromClauseItem(pstate, j->rarg,
										  &r_rte,
										  &r_rtindex,
										  &r_namespace);

		/* Remove the left-side RTEs from the namespace list again */
		pstate->p_namespace = list_truncate(pstate->p_namespace,
											sv_namespace_length);

		/*
		 * Check for conflicting refnames in left and right subtrees. Must do
		 * this because higher levels will assume I hand back a self-
		 * consistent namespace list.
		 */
		checkNameSpaceConflicts(pstate, l_namespace, r_namespace);

		/*
		 * Generate combined namespace info for possible use below.
		 */
		my_namespace = list_concat(l_namespace, r_namespace);

		/*
		 * Extract column name and var lists from both subtrees
		 *
		 * Note: expandRTE returns new lists, safe for me to modify
		 */
		expandRTE(l_rte, l_rtindex, 0, -1, false,
				  &l_colnames, &l_colvars);
		expandRTE(r_rte, r_rtindex, 0, -1, false,
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

			Assert(j->usingClause == NIL);		/* shouldn't have USING() too */

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

			j->usingClause = rlist;
		}

		/*
		 * Now transform the join qualifications, if any.
		 */
		res_colnames = NIL;
		res_colvars = NIL;

		if (j->usingClause)
		{
			/*
			 * JOIN/USING (or NATURAL JOIN, as transformed above). Transform
			 * the list into an explicit ON-condition, and generate a list of
			 * merged result columns.
			 */
			List	   *ucols = j->usingClause;
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
												l_rte,
												r_rte,
												l_usingvars,
												r_usingvars);
		}
		else if (j->quals)
		{
			/* User-written ON-condition; transform it */
			j->quals = transformJoinOnClause(pstate, j, my_namespace);
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

		/* make a matching link to the JoinExpr for later use */
		for (k = list_length(pstate->p_joinexprs) + 1; k < j->rtindex; k++)
			pstate->p_joinexprs = lappend(pstate->p_joinexprs, NULL);
		pstate->p_joinexprs = lappend(pstate->p_joinexprs, j);
		Assert(list_length(pstate->p_joinexprs) == j->rtindex);

		/*
		 * Prepare returned namespace list.  If the JOIN has an alias then it
		 * hides the contained RTEs completely; otherwise, the contained RTEs
		 * are still visible as table names, but are not visible for
		 * unqualified column-name access.
		 *
		 * Note: if there are nested alias-less JOINs, the lower-level ones
		 * will remain in the list although they have neither p_rel_visible
		 * nor p_cols_visible set.  We could delete such list items, but it's
		 * unclear that it's worth expending cycles to do so.
		 */
		if (j->alias != NULL)
			my_namespace = NIL;
		else
			setNamespaceColumnVisibility(my_namespace, false);

		/*
		 * The join RTE itself is always made visible for unqualified column
		 * names.  It's visible as a relation name only if it has an alias.
		 */
		*namespace = lappend(my_namespace,
							 makeNamespaceItem(rte,
											   (j->alias != NULL),
											   true,
											   false,
											   true));

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
		outcoltype = select_common_type(pstate,
										list_make2(l_colvar, r_colvar),
										"JOIN/USING",
										NULL);
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
							 COERCION_IMPLICIT, COERCE_IMPLICIT_CAST, -1);
	else if (l_colvar->vartypmod != outcoltypmod)
		l_node = (Node *) makeRelabelType((Expr *) l_colvar,
										  outcoltype, outcoltypmod,
										  InvalidOid,	/* fixed below */
										  COERCE_IMPLICIT_CAST);
	else
		l_node = (Node *) l_colvar;

	if (r_colvar->vartype != outcoltype)
		r_node = coerce_type(pstate, (Node *) r_colvar, r_colvar->vartype,
							 outcoltype, outcoltypmod,
							 COERCION_IMPLICIT, COERCE_IMPLICIT_CAST, -1);
	else if (r_colvar->vartypmod != outcoltypmod)
		r_node = (Node *) makeRelabelType((Expr *) r_colvar,
										  outcoltype, outcoltypmod,
										  InvalidOid,	/* fixed below */
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
				/* coalescecollid will get set below */
				c->args = list_make2(l_node, r_node);
				c->location = -1;
				res_node = (Node *) c;
				break;
			}
		default:
			elog(ERROR, "unrecognized join type: %d", (int) jointype);
			res_node = NULL;	/* keep compiler quiet */
			break;
	}

	/*
	 * Apply assign_expr_collations to fix up the collation info in the
	 * coercion and CoalesceExpr nodes, if we made any.  This must be done now
	 * so that the join node's alias vars show correct collation info.
	 */
	assign_expr_collations(pstate, res_node);

	return res_node;
}

/*
 * makeNamespaceItem -
 *	  Convenience subroutine to construct a ParseNamespaceItem.
 */
static ParseNamespaceItem *
makeNamespaceItem(RangeTblEntry *rte, bool rel_visible, bool cols_visible,
				  bool lateral_only, bool lateral_ok)
{
	ParseNamespaceItem *nsitem;

	nsitem = (ParseNamespaceItem *) palloc(sizeof(ParseNamespaceItem));
	nsitem->p_rte = rte;
	nsitem->p_rel_visible = rel_visible;
	nsitem->p_cols_visible = cols_visible;
	nsitem->p_lateral_only = lateral_only;
	nsitem->p_lateral_ok = lateral_ok;
	return nsitem;
}

/*
 * setNamespaceColumnVisibility -
 *	  Convenience subroutine to update cols_visible flags in a namespace list.
 */
static void
setNamespaceColumnVisibility(List *namespace, bool cols_visible)
{
	ListCell   *lc;

	foreach(lc, namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(lc);

		nsitem->p_cols_visible = cols_visible;
	}
}

/*
 * setNamespaceLateralState -
 *	  Convenience subroutine to update LATERAL flags in a namespace list.
 */
static void
setNamespaceLateralState(List *namespace, bool lateral_only, bool lateral_ok)
{
	ListCell   *lc;

	foreach(lc, namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(lc);

		nsitem->p_lateral_only = lateral_only;
		nsitem->p_lateral_ok = lateral_ok;
	}
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
					 ParseExprKind exprKind, const char *constructName)
{
	Node	   *qual;

	if (clause == NULL)
		return NULL;

	qual = transformExpr(pstate, clause, exprKind);

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
					 ParseExprKind exprKind, const char *constructName)
{
	Node	   *qual;

	if (clause == NULL)
		return NULL;

	qual = transformExpr(pstate, clause, exprKind);

	qual = coerce_to_specific_type(pstate, qual, INT8OID, constructName);

	/* LIMIT can't refer to any variables of the current query */
	checkExprIsVarFree(pstate, qual, constructName);

	return qual;
}

/*
 * checkExprIsVarFree
 *		Check that given expr has no Vars of the current query level
 *		(aggregates and window functions should have been rejected already).
 *
 * This is used to check expressions that have to have a consistent value
 * across all rows of the query, such as a LIMIT.  Arguably it should reject
 * volatile functions, too, but we don't do that --- whatever value the
 * function gives on first execution is what you get.
 *
 * constructName does not affect the semantics, but is used in error messages
 */
static void
checkExprIsVarFree(ParseState *pstate, Node *n, const char *constructName)
{
	if (contain_vars_of_level(n, 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
		/* translator: %s is name of a SQL construct, eg LIMIT */
				 errmsg("argument of %s must not contain variables",
						constructName),
				 parser_errposition(pstate,
									locate_var_of_level(n, 0))));
	}
}


/*
 * checkTargetlistEntrySQL92 -
 *	  Validate a targetlist entry found by findTargetlistEntrySQL92
 *
 * When we select a pre-existing tlist entry as a result of syntax such
 * as "GROUP BY 1", we have to make sure it is acceptable for use in the
 * indicated clause type; transformExpr() will have treated it as a regular
 * targetlist item.
 */
static void
checkTargetlistEntrySQL92(ParseState *pstate, TargetEntry *tle,
						  ParseExprKind exprKind)
{
	switch (exprKind)
	{
		case EXPR_KIND_GROUP_BY:
			/* reject aggregates and window functions */
			if (pstate->p_hasAggs &&
				contain_aggs_of_level((Node *) tle->expr, 0))
				ereport(ERROR,
						(errcode(ERRCODE_GROUPING_ERROR),
				/* translator: %s is name of a SQL construct, eg GROUP BY */
						 errmsg("aggregate functions are not allowed in %s",
								ParseExprKindName(exprKind)),
						 parser_errposition(pstate,
							   locate_agg_of_level((Node *) tle->expr, 0))));
			if (pstate->p_hasWindowFuncs &&
				contain_windowfuncs((Node *) tle->expr))
				ereport(ERROR,
						(errcode(ERRCODE_WINDOWING_ERROR),
				/* translator: %s is name of a SQL construct, eg GROUP BY */
						 errmsg("window functions are not allowed in %s",
								ParseExprKindName(exprKind)),
						 parser_errposition(pstate,
									locate_windowfunc((Node *) tle->expr))));
			break;
		case EXPR_KIND_ORDER_BY:
			/* no extra checks needed */
			break;
		case EXPR_KIND_DISTINCT_ON:
			/* no extra checks needed */
			break;
		default:
			elog(ERROR, "unexpected exprKind in checkTargetlistEntrySQL92");
			break;
	}
}

/*
 *	findTargetlistEntrySQL92 -
 *	  Returns the targetlist entry matching the given (untransformed) node.
 *	  If no matching entry exists, one is created and appended to the target
 *	  list as a "resjunk" node.
 *
 * This function supports the old SQL92 ORDER BY interpretation, where the
 * expression is an output column name or number.  If we fail to find a
 * match of that sort, we fall through to the SQL99 rules.  For historical
 * reasons, Postgres also allows this interpretation for GROUP BY, though
 * the standard never did.  However, for GROUP BY we prefer a SQL99 match.
 * This function is *not* used for WINDOW definitions.
 *
 * node		the ORDER BY, GROUP BY, or DISTINCT ON expression to be matched
 * tlist	the target list (passed by reference so we can append to it)
 * exprKind identifies clause type being processed
 */
static TargetEntry *
findTargetlistEntrySQL92(ParseState *pstate, Node *node, List **tlist,
						 ParseExprKind exprKind)
{
	ListCell   *tl;

	/*----------
	 * Handle two special cases as mandated by the SQL92 spec:
	 *
	 * 1. Bare ColumnName (no qualifier or subscripts)
	 *	  For a bare identifier, we search for a matching column name
	 *	  in the existing target list.  Multiple matches are an error
	 *	  unless they refer to identical values; for example,
	 *	  we allow	SELECT a, a FROM table ORDER BY a
	 *	  but not	SELECT a AS b, b FROM table ORDER BY b
	 *	  If no match is found, we fall through and treat the identifier
	 *	  as an expression.
	 *	  For GROUP BY, it is incorrect to match the grouping item against
	 *	  targetlist entries: according to SQL92, an identifier in GROUP BY
	 *	  is a reference to a column name exposed by FROM, not to a target
	 *	  list column.  However, many implementations (including pre-7.0
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
	 *	  actual constant, so this does not create a conflict with SQL99.
	 *	  GROUP BY column-number is not allowed by SQL92, but since
	 *	  the standard has no other behavior defined for this syntax,
	 *	  we may as well accept this common extension.
	 *
	 * Note that pre-existing resjunk targets must not be used in either case,
	 * since the user didn't write them in his SELECT list.
	 *
	 * If neither special case applies, fall through to treat the item as
	 * an expression per SQL99.
	 *----------
	 */
	if (IsA(node, ColumnRef) &&
		list_length(((ColumnRef *) node)->fields) == 1 &&
		IsA(linitial(((ColumnRef *) node)->fields), String))
	{
		char	   *name = strVal(linitial(((ColumnRef *) node)->fields));
		int			location = ((ColumnRef *) node)->location;

		if (exprKind == EXPR_KIND_GROUP_BY)
		{
			/*
			 * In GROUP BY, we must prefer a match against a FROM-clause
			 * column to one against the targetlist.  Look to see if there is
			 * a matching column.  If so, fall through to use SQL99 rules.
			 * NOTE: if name could refer ambiguously to more than one column
			 * name exposed by FROM, colNameToVar will ereport(ERROR). That's
			 * just what we want here.
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
			TargetEntry *target_result = NULL;

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
											ParseExprKindName(exprKind),
											name),
									 parser_errposition(pstate, location)));
					}
					else
						target_result = tle;
					/* Stay in loop to check for ambiguity */
				}
			}
			if (target_result != NULL)
			{
				/* return the first match, after suitable validation */
				checkTargetlistEntrySQL92(pstate, target_result, exprKind);
				return target_result;
			}
		}
	}
	if (IsA(node, A_Const))
	{
		Value	   *val = &((A_Const *) node)->val;
		int			location = ((A_Const *) node)->location;
		int			targetlist_pos = 0;
		int			target_pos;

		if (!IsA(val, Integer))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
			/* translator: %s is name of a SQL construct, eg ORDER BY */
					 errmsg("non-integer constant in %s",
							ParseExprKindName(exprKind)),
					 parser_errposition(pstate, location)));

		target_pos = intVal(val);
		foreach(tl, *tlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(tl);

			if (!tle->resjunk)
			{
				if (++targetlist_pos == target_pos)
				{
					/* return the unique match, after suitable validation */
					checkTargetlistEntrySQL92(pstate, tle, exprKind);
					return tle;
				}
			}
		}
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
		/* translator: %s is name of a SQL construct, eg ORDER BY */
				 errmsg("%s position %d is not in select list",
						ParseExprKindName(exprKind), target_pos),
				 parser_errposition(pstate, location)));
	}

	/*
	 * Otherwise, we have an expression, so process it per SQL99 rules.
	 */
	return findTargetlistEntrySQL99(pstate, node, tlist, exprKind);
}

/*
 *	findTargetlistEntrySQL99 -
 *	  Returns the targetlist entry matching the given (untransformed) node.
 *	  If no matching entry exists, one is created and appended to the target
 *	  list as a "resjunk" node.
 *
 * This function supports the SQL99 interpretation, wherein the expression
 * is just an ordinary expression referencing input column names.
 *
 * node		the ORDER BY, GROUP BY, etc expression to be matched
 * tlist	the target list (passed by reference so we can append to it)
 * exprKind identifies clause type being processed
 */
static TargetEntry *
findTargetlistEntrySQL99(ParseState *pstate, Node *node, List **tlist,
						 ParseExprKind exprKind)
{
	TargetEntry *target_result;
	ListCell   *tl;
	Node	   *expr;

	/*
	 * Convert the untransformed node to a transformed expression, and search
	 * for a match in the tlist.  NOTE: it doesn't really matter whether there
	 * is more than one match.  Also, we are willing to match an existing
	 * resjunk target here, though the SQL92 cases above must ignore resjunk
	 * targets.
	 */
	expr = transformExpr(pstate, node, exprKind);

	foreach(tl, *tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tl);
		Node	   *texpr;

		/*
		 * Ignore any implicit cast on the existing tlist expression.
		 *
		 * This essentially allows the ORDER/GROUP/etc item to adopt the same
		 * datatype previously selected for a textually-equivalent tlist item.
		 * There can't be any implicit cast at top level in an ordinary SELECT
		 * tlist at this stage, but the case does arise with ORDER BY in an
		 * aggregate function.
		 */
		texpr = strip_implicit_coercions((Node *) tle->expr);

		if (equal(expr, texpr))
			return tle;
	}

	/*
	 * If no matches, construct a new target entry which is appended to the
	 * end of the target list.  This target is given resjunk = TRUE so that it
	 * will not be projected into the final tuple.
	 */
	target_result = transformTargetEntry(pstate, node, expr, exprKind,
										 NULL, true);

	*tlist = lappend(*tlist, target_result);

	return target_result;
}

/*-------------------------------------------------------------------------
 * Flatten out parenthesized sublists in grouping lists, and some cases
 * of nested grouping sets.
 *
 * Inside a grouping set (ROLLUP, CUBE, or GROUPING SETS), we expect the
 * content to be nested no more than 2 deep: i.e. ROLLUP((a,b),(c,d)) is
 * ok, but ROLLUP((a,(b,c)),d) is flattened to ((a,b,c),d), which we then
 * (later) normalize to ((a,b,c),(d)).
 *
 * CUBE or ROLLUP can be nested inside GROUPING SETS (but not the reverse),
 * and we leave that alone if we find it. But if we see GROUPING SETS inside
 * GROUPING SETS, we can flatten and normalize as follows:
 *	 GROUPING SETS (a, (b,c), GROUPING SETS ((c,d),(e)), (f,g))
 * becomes
 *	 GROUPING SETS ((a), (b,c), (c,d), (e), (f,g))
 *
 * This is per the spec's syntax transformations, but these are the only such
 * transformations we do in parse analysis, so that queries retain the
 * originally specified grouping set syntax for CUBE and ROLLUP as much as
 * possible when deparsed. (Full expansion of the result into a list of
 * grouping sets is left to the planner.)
 *
 * When we're done, the resulting list should contain only these possible
 * elements:
 *	 - an expression
 *	 - a CUBE or ROLLUP with a list of expressions nested 2 deep
 *	 - a GROUPING SET containing any of:
 *		- expression lists
 *		- empty grouping sets
 *		- CUBE or ROLLUP nodes with lists nested 2 deep
 * The return is a new list, but doesn't deep-copy the old nodes except for
 * GroupingSet nodes.
 *
 * As a side effect, flag whether the list has any GroupingSet nodes.
 *-------------------------------------------------------------------------
 */
static Node *
flatten_grouping_sets(Node *expr, bool toplevel, bool *hasGroupingSets)
{
	/* just in case of pathological input */
	check_stack_depth();

	if (expr == (Node *) NIL)
		return (Node *) NIL;

	switch (expr->type)
	{
		case T_RowExpr:
			{
				RowExpr    *r = (RowExpr *) expr;

				if (r->row_format == COERCE_IMPLICIT_CAST)
					return flatten_grouping_sets((Node *) r->args,
												 false, NULL);
			}
			break;
		case T_GroupingSet:
			{
				GroupingSet *gset = (GroupingSet *) expr;
				ListCell   *l2;
				List	   *result_set = NIL;

				if (hasGroupingSets)
					*hasGroupingSets = true;

				/*
				 * at the top level, we skip over all empty grouping sets; the
				 * caller can supply the canonical GROUP BY () if nothing is
				 * left.
				 */

				if (toplevel && gset->kind == GROUPING_SET_EMPTY)
					return (Node *) NIL;

				foreach(l2, gset->content)
				{
					Node	   *n1 = lfirst(l2);
					Node	   *n2 = flatten_grouping_sets(n1, false, NULL);

					if (IsA(n1, GroupingSet) &&
						((GroupingSet *) n1)->kind == GROUPING_SET_SETS)
					{
						result_set = list_concat(result_set, (List *) n2);
					}
					else
						result_set = lappend(result_set, n2);
				}

				/*
				 * At top level, keep the grouping set node; but if we're in a
				 * nested grouping set, then we need to concat the flattened
				 * result into the outer list if it's simply nested.
				 */

				if (toplevel || (gset->kind != GROUPING_SET_SETS))
				{
					return (Node *) makeGroupingSet(gset->kind, result_set, gset->location);
				}
				else
					return (Node *) result_set;
			}
		case T_List:
			{
				List	   *result = NIL;
				ListCell   *l;

				foreach(l, (List *) expr)
				{
					Node	   *n = flatten_grouping_sets(lfirst(l), toplevel, hasGroupingSets);

					if (n != (Node *) NIL)
					{
						if (IsA(n, List))
							result = list_concat(result, (List *) n);
						else
							result = lappend(result, n);
					}
				}

				return (Node *) result;
			}
		default:
			break;
	}

	return expr;
}

/*
 * Transform a single expression within a GROUP BY clause or grouping set.
 *
 * The expression is added to the targetlist if not already present, and to the
 * flatresult list (which will become the groupClause) if not already present
 * there.  The sortClause is consulted for operator and sort order hints.
 *
 * Returns the ressortgroupref of the expression.
 *
 * flatresult	reference to flat list of SortGroupClause nodes
 * seen_local	bitmapset of sortgrouprefs already seen at the local level
 * pstate		ParseState
 * gexpr		node to transform
 * targetlist	reference to TargetEntry list
 * sortClause	ORDER BY clause (SortGroupClause nodes)
 * exprKind		expression kind
 * useSQL99		SQL99 rather than SQL92 syntax
 * toplevel		false if within any grouping set
 */
static Index
transformGroupClauseExpr(List **flatresult, Bitmapset *seen_local,
						 ParseState *pstate, Node *gexpr,
						 List **targetlist, List *sortClause,
						 ParseExprKind exprKind, bool useSQL99, bool toplevel)
{
	TargetEntry *tle;
	bool		found = false;

	if (useSQL99)
		tle = findTargetlistEntrySQL99(pstate, gexpr,
									   targetlist, exprKind);
	else
		tle = findTargetlistEntrySQL92(pstate, gexpr,
									   targetlist, exprKind);

	if (tle->ressortgroupref > 0)
	{
		ListCell   *sl;

		/*
		 * Eliminate duplicates (GROUP BY x, x) but only at local level.
		 * (Duplicates in grouping sets can affect the number of returned
		 * rows, so can't be dropped indiscriminately.)
		 *
		 * Since we don't care about anything except the sortgroupref, we can
		 * use a bitmapset rather than scanning lists.
		 */
		if (bms_is_member(tle->ressortgroupref, seen_local))
			return 0;

		/*
		 * If we're already in the flat clause list, we don't need to consider
		 * adding ourselves again.
		 */
		found = targetIsInSortList(tle, InvalidOid, *flatresult);
		if (found)
			return tle->ressortgroupref;

		/*
		 * If the GROUP BY tlist entry also appears in ORDER BY, copy operator
		 * info from the (first) matching ORDER BY item.  This means that if
		 * you write something like "GROUP BY foo ORDER BY foo USING <<<", the
		 * GROUP BY operation silently takes on the equality semantics implied
		 * by the ORDER BY.  There are two reasons to do this: it improves the
		 * odds that we can implement both GROUP BY and ORDER BY with a single
		 * sort step, and it allows the user to choose the equality semantics
		 * used by GROUP BY, should she be working with a datatype that has
		 * more than one equality operator.
		 *
		 * If we're in a grouping set, though, we force our requested ordering
		 * to be NULLS LAST, because if we have any hope of using a sorted agg
		 * for the job, we're going to be tacking on generated NULL values
		 * after the corresponding groups. If the user demands nulls first,
		 * another sort step is going to be inevitable, but that's the
		 * planner's problem.
		 */

		foreach(sl, sortClause)
		{
			SortGroupClause *sc = (SortGroupClause *) lfirst(sl);

			if (sc->tleSortGroupRef == tle->ressortgroupref)
			{
				SortGroupClause *grpc = copyObject(sc);

				if (!toplevel)
					grpc->nulls_first = false;
				*flatresult = lappend(*flatresult, grpc);
				found = true;
				break;
			}
		}
	}

	/*
	 * If no match in ORDER BY, just add it to the result using default
	 * sort/group semantics.
	 */
	if (!found)
		*flatresult = addTargetToGroupList(pstate, tle,
										   *flatresult, *targetlist,
										   exprLocation(gexpr),
										   true);

	/*
	 * _something_ must have assigned us a sortgroupref by now...
	 */

	return tle->ressortgroupref;
}

/*
 * Transform a list of expressions within a GROUP BY clause or grouping set.
 *
 * The list of expressions belongs to a single clause within which duplicates
 * can be safely eliminated.
 *
 * Returns an integer list of ressortgroupref values.
 *
 * flatresult	reference to flat list of SortGroupClause nodes
 * pstate		ParseState
 * list			nodes to transform
 * targetlist	reference to TargetEntry list
 * sortClause	ORDER BY clause (SortGroupClause nodes)
 * exprKind		expression kind
 * useSQL99		SQL99 rather than SQL92 syntax
 * toplevel		false if within any grouping set
 */
static List *
transformGroupClauseList(List **flatresult,
						 ParseState *pstate, List *list,
						 List **targetlist, List *sortClause,
						 ParseExprKind exprKind, bool useSQL99, bool toplevel)
{
	Bitmapset  *seen_local = NULL;
	List	   *result = NIL;
	ListCell   *gl;

	foreach(gl, list)
	{
		Node	   *gexpr = (Node *) lfirst(gl);

		Index		ref = transformGroupClauseExpr(flatresult,
												   seen_local,
												   pstate,
												   gexpr,
												   targetlist,
												   sortClause,
												   exprKind,
												   useSQL99,
												   toplevel);

		if (ref > 0)
		{
			seen_local = bms_add_member(seen_local, ref);
			result = lappend_int(result, ref);
		}
	}

	return result;
}

/*
 * Transform a grouping set and (recursively) its content.
 *
 * The grouping set might be a GROUPING SETS node with other grouping sets
 * inside it, but SETS within SETS have already been flattened out before
 * reaching here.
 *
 * Returns the transformed node, which now contains SIMPLE nodes with lists
 * of ressortgrouprefs rather than expressions.
 *
 * flatresult	reference to flat list of SortGroupClause nodes
 * pstate		ParseState
 * gset			grouping set to transform
 * targetlist	reference to TargetEntry list
 * sortClause	ORDER BY clause (SortGroupClause nodes)
 * exprKind		expression kind
 * useSQL99		SQL99 rather than SQL92 syntax
 * toplevel		false if within any grouping set
 */
static Node *
transformGroupingSet(List **flatresult,
					 ParseState *pstate, GroupingSet *gset,
					 List **targetlist, List *sortClause,
					 ParseExprKind exprKind, bool useSQL99, bool toplevel)
{
	ListCell   *gl;
	List	   *content = NIL;

	Assert(toplevel || gset->kind != GROUPING_SET_SETS);

	foreach(gl, gset->content)
	{
		Node	   *n = lfirst(gl);

		if (IsA(n, List))
		{
			List	   *l = transformGroupClauseList(flatresult,
													 pstate, (List *) n,
													 targetlist, sortClause,
												  exprKind, useSQL99, false);

			content = lappend(content, makeGroupingSet(GROUPING_SET_SIMPLE,
													   l,
													   exprLocation(n)));
		}
		else if (IsA(n, GroupingSet))
		{
			GroupingSet *gset2 = (GroupingSet *) lfirst(gl);

			content = lappend(content, transformGroupingSet(flatresult,
															pstate, gset2,
													  targetlist, sortClause,
												 exprKind, useSQL99, false));
		}
		else
		{
			Index		ref = transformGroupClauseExpr(flatresult,
													   NULL,
													   pstate,
													   n,
													   targetlist,
													   sortClause,
													   exprKind,
													   useSQL99,
													   false);

			content = lappend(content, makeGroupingSet(GROUPING_SET_SIMPLE,
													   list_make1_int(ref),
													   exprLocation(n)));
		}
	}

	/* Arbitrarily cap the size of CUBE, which has exponential growth */
	if (gset->kind == GROUPING_SET_CUBE)
	{
		if (list_length(content) > 12)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("CUBE is limited to 12 elements"),
					 parser_errposition(pstate, gset->location)));
	}

	return (Node *) makeGroupingSet(gset->kind, content, gset->location);
}


/*
 * transformGroupClause -
 *	  transform a GROUP BY clause
 *
 * GROUP BY items will be added to the targetlist (as resjunk columns)
 * if not already present, so the targetlist must be passed by reference.
 *
 * This is also used for window PARTITION BY clauses (which act almost the
 * same, but are always interpreted per SQL99 rules).
 *
 * Grouping sets make this a lot more complex than it was. Our goal here is
 * twofold: we make a flat list of SortGroupClause nodes referencing each
 * distinct expression used for grouping, with those expressions added to the
 * targetlist if needed. At the same time, we build the groupingSets tree,
 * which stores only ressortgrouprefs as integer lists inside GroupingSet nodes
 * (possibly nested, but limited in depth: a GROUPING_SET_SETS node can contain
 * nested SIMPLE, CUBE or ROLLUP nodes, but not more sets - we flatten that
 * out; while CUBE and ROLLUP can contain only SIMPLE nodes).
 *
 * We skip much of the hard work if there are no grouping sets.
 *
 * One subtlety is that the groupClause list can end up empty while the
 * groupingSets list is not; this happens if there are only empty grouping
 * sets, or an explicit GROUP BY (). This has the same effect as specifying
 * aggregates or a HAVING clause with no GROUP BY; the output is one row per
 * grouping set even if the input is empty.
 *
 * Returns the transformed (flat) groupClause.
 *
 * pstate		ParseState
 * grouplist	clause to transform
 * groupingSets reference to list to contain the grouping set tree
 * targetlist	reference to TargetEntry list
 * sortClause	ORDER BY clause (SortGroupClause nodes)
 * exprKind		expression kind
 * useSQL99		SQL99 rather than SQL92 syntax
 */
List *
transformGroupClause(ParseState *pstate, List *grouplist, List **groupingSets,
					 List **targetlist, List *sortClause,
					 ParseExprKind exprKind, bool useSQL99)
{
	List	   *result = NIL;
	List	   *flat_grouplist;
	List	   *gsets = NIL;
	ListCell   *gl;
	bool		hasGroupingSets = false;
	Bitmapset  *seen_local = NULL;

	/*
	 * Recursively flatten implicit RowExprs. (Technically this is only needed
	 * for GROUP BY, per the syntax rules for grouping sets, but we do it
	 * anyway.)
	 */
	flat_grouplist = (List *) flatten_grouping_sets((Node *) grouplist,
													true,
													&hasGroupingSets);

	/*
	 * If the list is now empty, but hasGroupingSets is true, it's because we
	 * elided redundant empty grouping sets. Restore a single empty grouping
	 * set to leave a canonical form: GROUP BY ()
	 */

	if (flat_grouplist == NIL && hasGroupingSets)
	{
		flat_grouplist = list_make1(makeGroupingSet(GROUPING_SET_EMPTY,
													NIL,
										  exprLocation((Node *) grouplist)));
	}

	foreach(gl, flat_grouplist)
	{
		Node	   *gexpr = (Node *) lfirst(gl);

		if (IsA(gexpr, GroupingSet))
		{
			GroupingSet *gset = (GroupingSet *) gexpr;

			switch (gset->kind)
			{
				case GROUPING_SET_EMPTY:
					gsets = lappend(gsets, gset);
					break;
				case GROUPING_SET_SIMPLE:
					/* can't happen */
					Assert(false);
					break;
				case GROUPING_SET_SETS:
				case GROUPING_SET_CUBE:
				case GROUPING_SET_ROLLUP:
					gsets = lappend(gsets,
									transformGroupingSet(&result,
														 pstate, gset,
													  targetlist, sortClause,
												  exprKind, useSQL99, true));
					break;
			}
		}
		else
		{
			Index		ref = transformGroupClauseExpr(&result, seen_local,
													   pstate, gexpr,
													   targetlist, sortClause,
												   exprKind, useSQL99, true);

			if (ref > 0)
			{
				seen_local = bms_add_member(seen_local, ref);
				if (hasGroupingSets)
					gsets = lappend(gsets,
									makeGroupingSet(GROUPING_SET_SIMPLE,
													list_make1_int(ref),
													exprLocation(gexpr)));
			}
		}
	}

	/* parser should prevent this */
	Assert(gsets == NIL || groupingSets != NULL);

	if (groupingSets)
		*groupingSets = gsets;

	return result;
}

/*
 * transformSortClause -
 *	  transform an ORDER BY clause
 *
 * ORDER BY items will be added to the targetlist (as resjunk columns)
 * if not already present, so the targetlist must be passed by reference.
 *
 * This is also used for window and aggregate ORDER BY clauses (which act
 * almost the same, but are always interpreted per SQL99 rules).
 */
List *
transformSortClause(ParseState *pstate,
					List *orderlist,
					List **targetlist,
					ParseExprKind exprKind,
					bool resolveUnknown,
					bool useSQL99)
{
	List	   *sortlist = NIL;
	ListCell   *olitem;

	foreach(olitem, orderlist)
	{
		SortBy	   *sortby = (SortBy *) lfirst(olitem);
		TargetEntry *tle;

		if (useSQL99)
			tle = findTargetlistEntrySQL99(pstate, sortby->node,
										   targetlist, exprKind);
		else
			tle = findTargetlistEntrySQL92(pstate, sortby->node,
										   targetlist, exprKind);

		sortlist = addTargetToSortList(pstate, tle,
									   sortlist, *targetlist, sortby,
									   resolveUnknown);
	}

	return sortlist;
}

/*
 * transformWindowDefinitions -
 *		transform window definitions (WindowDef to WindowClause)
 */
List *
transformWindowDefinitions(ParseState *pstate,
						   List *windowdefs,
						   List **targetlist)
{
	List	   *result = NIL;
	Index		winref = 0;
	ListCell   *lc;

	foreach(lc, windowdefs)
	{
		WindowDef  *windef = (WindowDef *) lfirst(lc);
		WindowClause *refwc = NULL;
		List	   *partitionClause;
		List	   *orderClause;
		WindowClause *wc;

		winref++;

		/*
		 * Check for duplicate window names.
		 */
		if (windef->name &&
			findWindowClause(result, windef->name) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_WINDOWING_ERROR),
					 errmsg("window \"%s\" is already defined", windef->name),
					 parser_errposition(pstate, windef->location)));

		/*
		 * If it references a previous window, look that up.
		 */
		if (windef->refname)
		{
			refwc = findWindowClause(result, windef->refname);
			if (refwc == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("window \"%s\" does not exist",
								windef->refname),
						 parser_errposition(pstate, windef->location)));
		}

		/*
		 * Transform PARTITION and ORDER specs, if any.  These are treated
		 * almost exactly like top-level GROUP BY and ORDER BY clauses,
		 * including the special handling of nondefault operator semantics.
		 */
		orderClause = transformSortClause(pstate,
										  windef->orderClause,
										  targetlist,
										  EXPR_KIND_WINDOW_ORDER,
										  true /* fix unknowns */ ,
										  true /* force SQL99 rules */ );
		partitionClause = transformGroupClause(pstate,
											   windef->partitionClause,
											   NULL,
											   targetlist,
											   orderClause,
											   EXPR_KIND_WINDOW_PARTITION,
											   true /* force SQL99 rules */ );

		/*
		 * And prepare the new WindowClause.
		 */
		wc = makeNode(WindowClause);
		wc->name = windef->name;
		wc->refname = windef->refname;

		/*
		 * Per spec, a windowdef that references a previous one copies the
		 * previous partition clause (and mustn't specify its own).  It can
		 * specify its own ordering clause, but only if the previous one had
		 * none.  It always specifies its own frame clause, and the previous
		 * one must not have a frame clause.  Yeah, it's bizarre that each of
		 * these cases works differently, but SQL:2008 says so; see 7.11
		 * <window clause> syntax rule 10 and general rule 1.  The frame
		 * clause rule is especially bizarre because it makes "OVER foo"
		 * different from "OVER (foo)", and requires the latter to throw an
		 * error if foo has a nondefault frame clause.  Well, ours not to
		 * reason why, but we do go out of our way to throw a useful error
		 * message for such cases.
		 */
		if (refwc)
		{
			if (partitionClause)
				ereport(ERROR,
						(errcode(ERRCODE_WINDOWING_ERROR),
				errmsg("cannot override PARTITION BY clause of window \"%s\"",
					   windef->refname),
						 parser_errposition(pstate, windef->location)));
			wc->partitionClause = copyObject(refwc->partitionClause);
		}
		else
			wc->partitionClause = partitionClause;
		if (refwc)
		{
			if (orderClause && refwc->orderClause)
				ereport(ERROR,
						(errcode(ERRCODE_WINDOWING_ERROR),
				   errmsg("cannot override ORDER BY clause of window \"%s\"",
						  windef->refname),
						 parser_errposition(pstate, windef->location)));
			if (orderClause)
			{
				wc->orderClause = orderClause;
				wc->copiedOrder = false;
			}
			else
			{
				wc->orderClause = copyObject(refwc->orderClause);
				wc->copiedOrder = true;
			}
		}
		else
		{
			wc->orderClause = orderClause;
			wc->copiedOrder = false;
		}
		if (refwc && refwc->frameOptions != FRAMEOPTION_DEFAULTS)
		{
			/*
			 * Use this message if this is a WINDOW clause, or if it's an OVER
			 * clause that includes ORDER BY or framing clauses.  (We already
			 * rejected PARTITION BY above, so no need to check that.)
			 */
			if (windef->name ||
				orderClause || windef->frameOptions != FRAMEOPTION_DEFAULTS)
				ereport(ERROR,
						(errcode(ERRCODE_WINDOWING_ERROR),
						 errmsg("cannot copy window \"%s\" because it has a frame clause",
								windef->refname),
						 parser_errposition(pstate, windef->location)));
			/* Else this clause is just OVER (foo), so say this: */
			ereport(ERROR,
					(errcode(ERRCODE_WINDOWING_ERROR),
			errmsg("cannot copy window \"%s\" because it has a frame clause",
				   windef->refname),
					 errhint("Omit the parentheses in this OVER clause."),
					 parser_errposition(pstate, windef->location)));
		}
		wc->frameOptions = windef->frameOptions;
		/* Process frame offset expressions */
		wc->startOffset = transformFrameOffset(pstate, wc->frameOptions,
											   windef->startOffset);
		wc->endOffset = transformFrameOffset(pstate, wc->frameOptions,
											 windef->endOffset);
		wc->winref = winref;

		result = lappend(result, wc);
	}

	return result;
}

/*
 * transformDistinctClause -
 *	  transform a DISTINCT clause
 *
 * Since we may need to add items to the query's targetlist, that list
 * is passed by reference.
 *
 * As with GROUP BY, we absorb the sorting semantics of ORDER BY as much as
 * possible into the distinctClause.  This avoids a possible need to re-sort,
 * and allows the user to choose the equality semantics used by DISTINCT,
 * should she be working with a datatype that has more than one equality
 * operator.
 *
 * is_agg is true if we are transforming an aggregate(DISTINCT ...)
 * function call.  This does not affect any behavior, only the phrasing
 * of error messages.
 */
List *
transformDistinctClause(ParseState *pstate,
						List **targetlist, List *sortClause, bool is_agg)
{
	List	   *result = NIL;
	ListCell   *slitem;
	ListCell   *tlitem;

	/*
	 * The distinctClause should consist of all ORDER BY items followed by all
	 * other non-resjunk targetlist items.  There must not be any resjunk
	 * ORDER BY items --- that would imply that we are sorting by a value that
	 * isn't necessarily unique within a DISTINCT group, so the results
	 * wouldn't be well-defined.  This construction ensures we follow the rule
	 * that sortClause and distinctClause match; in fact the sortClause will
	 * always be a prefix of distinctClause.
	 *
	 * Note a corner case: the same TLE could be in the ORDER BY list multiple
	 * times with different sortops.  We have to include it in the
	 * distinctClause the same way to preserve the prefix property. The net
	 * effect will be that the TLE value will be made unique according to both
	 * sortops.
	 */
	foreach(slitem, sortClause)
	{
		SortGroupClause *scl = (SortGroupClause *) lfirst(slitem);
		TargetEntry *tle = get_sortgroupclause_tle(scl, *targetlist);

		if (tle->resjunk)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 is_agg ?
					 errmsg("in an aggregate with DISTINCT, ORDER BY expressions must appear in argument list") :
					 errmsg("for SELECT DISTINCT, ORDER BY expressions must appear in select list"),
					 parser_errposition(pstate,
										exprLocation((Node *) tle->expr))));
		result = lappend(result, copyObject(scl));
	}

	/*
	 * Now add any remaining non-resjunk tlist items, using default sort/group
	 * semantics for their data types.
	 */
	foreach(tlitem, *targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tlitem);

		if (tle->resjunk)
			continue;			/* ignore junk */
		result = addTargetToGroupList(pstate, tle,
									  result, *targetlist,
									  exprLocation((Node *) tle->expr),
									  true);
	}

	/*
	 * Complain if we found nothing to make DISTINCT.  Returning an empty list
	 * would cause the parsed Query to look like it didn't have DISTINCT, with
	 * results that would probably surprise the user.  Note: this case is
	 * presently impossible for aggregates because of grammar restrictions,
	 * but we check anyway.
	 */
	if (result == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 is_agg ?
		errmsg("an aggregate with DISTINCT must have at least one argument") :
				 errmsg("SELECT DISTINCT must have at least one column")));

	return result;
}

/*
 * transformDistinctOnClause -
 *	  transform a DISTINCT ON clause
 *
 * Since we may need to add items to the query's targetlist, that list
 * is passed by reference.
 *
 * As with GROUP BY, we absorb the sorting semantics of ORDER BY as much as
 * possible into the distinctClause.  This avoids a possible need to re-sort,
 * and allows the user to choose the equality semantics used by DISTINCT,
 * should she be working with a datatype that has more than one equality
 * operator.
 */
List *
transformDistinctOnClause(ParseState *pstate, List *distinctlist,
						  List **targetlist, List *sortClause)
{
	List	   *result = NIL;
	List	   *sortgrouprefs = NIL;
	bool		skipped_sortitem;
	ListCell   *lc;
	ListCell   *lc2;

	/*
	 * Add all the DISTINCT ON expressions to the tlist (if not already
	 * present, they are added as resjunk items).  Assign sortgroupref numbers
	 * to them, and make a list of these numbers.  (NB: we rely below on the
	 * sortgrouprefs list being one-for-one with the original distinctlist.
	 * Also notice that we could have duplicate DISTINCT ON expressions and
	 * hence duplicate entries in sortgrouprefs.)
	 */
	foreach(lc, distinctlist)
	{
		Node	   *dexpr = (Node *) lfirst(lc);
		int			sortgroupref;
		TargetEntry *tle;

		tle = findTargetlistEntrySQL92(pstate, dexpr, targetlist,
									   EXPR_KIND_DISTINCT_ON);
		sortgroupref = assignSortGroupRef(tle, *targetlist);
		sortgrouprefs = lappend_int(sortgrouprefs, sortgroupref);
	}

	/*
	 * If the user writes both DISTINCT ON and ORDER BY, adopt the sorting
	 * semantics from ORDER BY items that match DISTINCT ON items, and also
	 * adopt their column sort order.  We insist that the distinctClause and
	 * sortClause match, so throw error if we find the need to add any more
	 * distinctClause items after we've skipped an ORDER BY item that wasn't
	 * in DISTINCT ON.
	 */
	skipped_sortitem = false;
	foreach(lc, sortClause)
	{
		SortGroupClause *scl = (SortGroupClause *) lfirst(lc);

		if (list_member_int(sortgrouprefs, scl->tleSortGroupRef))
		{
			if (skipped_sortitem)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("SELECT DISTINCT ON expressions must match initial ORDER BY expressions"),
						 parser_errposition(pstate,
								  get_matching_location(scl->tleSortGroupRef,
														sortgrouprefs,
														distinctlist))));
			else
				result = lappend(result, copyObject(scl));
		}
		else
			skipped_sortitem = true;
	}

	/*
	 * Now add any remaining DISTINCT ON items, using default sort/group
	 * semantics for their data types.  (Note: this is pretty questionable; if
	 * the ORDER BY list doesn't include all the DISTINCT ON items and more
	 * besides, you certainly aren't using DISTINCT ON in the intended way,
	 * and you probably aren't going to get consistent results.  It might be
	 * better to throw an error or warning here.  But historically we've
	 * allowed it, so keep doing so.)
	 */
	forboth(lc, distinctlist, lc2, sortgrouprefs)
	{
		Node	   *dexpr = (Node *) lfirst(lc);
		int			sortgroupref = lfirst_int(lc2);
		TargetEntry *tle = get_sortgroupref_tle(sortgroupref, *targetlist);

		if (targetIsInSortList(tle, InvalidOid, result))
			continue;			/* already in list (with some semantics) */
		if (skipped_sortitem)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 errmsg("SELECT DISTINCT ON expressions must match initial ORDER BY expressions"),
					 parser_errposition(pstate, exprLocation(dexpr))));
		result = addTargetToGroupList(pstate, tle,
									  result, *targetlist,
									  exprLocation(dexpr),
									  true);
	}

	/*
	 * An empty result list is impossible here because of grammar
	 * restrictions.
	 */
	Assert(result != NIL);

	return result;
}

/*
 * get_matching_location
 *		Get the exprLocation of the exprs member corresponding to the
 *		(first) member of sortgrouprefs that equals sortgroupref.
 *
 * This is used so that we can point at a troublesome DISTINCT ON entry.
 * (Note that we need to use the original untransformed DISTINCT ON list
 * item, as whatever TLE it corresponds to will very possibly have a
 * parse location pointing to some matching entry in the SELECT list
 * or ORDER BY list.)
 */
static int
get_matching_location(int sortgroupref, List *sortgrouprefs, List *exprs)
{
	ListCell   *lcs;
	ListCell   *lce;

	forboth(lcs, sortgrouprefs, lce, exprs)
	{
		if (lfirst_int(lcs) == sortgroupref)
			return exprLocation((Node *) lfirst(lce));
	}
	/* if no match, caller blew it */
	elog(ERROR, "get_matching_location: no matching sortgroupref");
	return -1;					/* keep compiler quiet */
}

/*
 * resolve_unique_index_expr
 *		Infer a unique index from a list of indexElems, for ON
 *		CONFLICT clause
 *
 * Perform parse analysis of expressions and columns appearing within ON
 * CONFLICT clause.  During planning, the returned list of expressions is used
 * to infer which unique index to use.
 */
static List *
resolve_unique_index_expr(ParseState *pstate, InferClause *infer,
						  Relation heapRel)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, infer->indexElems)
	{
		IndexElem  *ielem = (IndexElem *) lfirst(l);
		InferenceElem *pInfer = makeNode(InferenceElem);
		Node	   *parse;

		/*
		 * Raw grammar re-uses CREATE INDEX infrastructure for unique index
		 * inference clause, and so will accept opclasses by name and so on.
		 *
		 * Make no attempt to match ASC or DESC ordering or NULLS FIRST/NULLS
		 * LAST ordering, since those are not significant for inference
		 * purposes (any unique index matching the inference specification in
		 * other regards is accepted indifferently).  Actively reject this as
		 * wrong-headed.
		 */
		if (ielem->ordering != SORTBY_DEFAULT)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 errmsg("ASC/DESC is not allowed in ON CONFLICT clause"),
					 parser_errposition(pstate,
										exprLocation((Node *) infer))));
		if (ielem->nulls_ordering != SORTBY_NULLS_DEFAULT)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
			 errmsg("NULLS FIRST/LAST is not allowed in ON CONFLICT clause"),
					 parser_errposition(pstate,
										exprLocation((Node *) infer))));

		if (!ielem->expr)
		{
			/* Simple index attribute */
			ColumnRef  *n;

			/*
			 * Grammar won't have built raw expression for us in event of
			 * plain column reference.  Create one directly, and perform
			 * expression transformation.  Planner expects this, and performs
			 * its own normalization for the purposes of matching against
			 * pg_index.
			 */
			n = makeNode(ColumnRef);
			n->fields = list_make1(makeString(ielem->name));
			/* Location is approximately that of inference specification */
			n->location = infer->location;
			parse = (Node *) n;
		}
		else
		{
			/* Do parse transformation of the raw expression */
			parse = (Node *) ielem->expr;
		}

		/*
		 * transformExpr() should have already rejected subqueries,
		 * aggregates, and window functions, based on the EXPR_KIND_ for an
		 * index expression.  Expressions returning sets won't have been
		 * rejected, but don't bother doing so here; there should be no
		 * available expression unique index to match any such expression
		 * against anyway.
		 */
		pInfer->expr = transformExpr(pstate, parse, EXPR_KIND_INDEX_EXPRESSION);

		/* Perform lookup of collation and operator class as required */
		if (!ielem->collation)
			pInfer->infercollid = InvalidOid;
		else
			pInfer->infercollid = LookupCollation(pstate, ielem->collation,
												  exprLocation(pInfer->expr));

		if (!ielem->opclass)
			pInfer->inferopclass = InvalidOid;
		else
			pInfer->inferopclass = get_opclass_oid(BTREE_AM_OID,
												   ielem->opclass, false);

		result = lappend(result, pInfer);
	}

	return result;
}

/*
 * transformOnConflictArbiter -
 *		transform arbiter expressions in an ON CONFLICT clause.
 *
 * Transformed expressions used to infer one unique index relation to serve as
 * an ON CONFLICT arbiter.  Partial unique indexes may be inferred using WHERE
 * clause from inference specification clause.
 */
void
transformOnConflictArbiter(ParseState *pstate,
						   OnConflictClause *onConflictClause,
						   List **arbiterExpr, Node **arbiterWhere,
						   Oid *constraint)
{
	InferClause *infer = onConflictClause->infer;

	*arbiterExpr = NIL;
	*arbiterWhere = NULL;
	*constraint = InvalidOid;

	if (onConflictClause->action == ONCONFLICT_UPDATE && !infer)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("ON CONFLICT DO UPDATE requires inference specification or constraint name"),
				 errhint("For example, ON CONFLICT (column_name)."),
				 parser_errposition(pstate,
								  exprLocation((Node *) onConflictClause))));

	/*
	 * To simplify certain aspects of its design, speculative insertion into
	 * system catalogs is disallowed
	 */
	if (IsCatalogRelation(pstate->p_target_relation))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		   errmsg("ON CONFLICT is not supported with system catalog tables"),
				 parser_errposition(pstate,
								  exprLocation((Node *) onConflictClause))));

	/* Same applies to table used by logical decoding as catalog table */
	if (RelationIsUsedAsCatalogTable(pstate->p_target_relation))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ON CONFLICT is not supported on table \"%s\" used as a catalog table",
						RelationGetRelationName(pstate->p_target_relation)),
				 parser_errposition(pstate,
								  exprLocation((Node *) onConflictClause))));

	/* ON CONFLICT DO NOTHING does not require an inference clause */
	if (infer)
	{
		List	   *save_namespace;

		/*
		 * While we process the arbiter expressions, accept only non-qualified
		 * references to the target table. Hide any other relations.
		 */
		save_namespace = pstate->p_namespace;
		pstate->p_namespace = NIL;
		addRTEtoQuery(pstate, pstate->p_target_rangetblentry,
					  false, false, true);

		if (infer->indexElems)
			*arbiterExpr = resolve_unique_index_expr(pstate, infer,
												  pstate->p_target_relation);

		/*
		 * Handling inference WHERE clause (for partial unique index
		 * inference)
		 */
		if (infer->whereClause)
			*arbiterWhere = transformExpr(pstate, infer->whereClause,
										  EXPR_KIND_INDEX_PREDICATE);

		pstate->p_namespace = save_namespace;

		if (infer->conname)
			*constraint = get_relation_constraint_oid(RelationGetRelid(pstate->p_target_relation),
													  infer->conname, false);
	}

	/*
	 * It's convenient to form a list of expressions based on the
	 * representation used by CREATE INDEX, since the same restrictions are
	 * appropriate (e.g. on subqueries).  However, from here on, a dedicated
	 * primnode representation is used for inference elements, and so
	 * assign_query_collations() can be trusted to do the right thing with the
	 * post parse analysis query tree inference clause representation.
	 */
}

/*
 * addTargetToSortList
 *		If the given targetlist entry isn't already in the SortGroupClause
 *		list, add it to the end of the list, using the given sort ordering
 *		info.
 *
 * If resolveUnknown is TRUE, convert TLEs of type UNKNOWN to TEXT.  If not,
 * do nothing (which implies the search for a sort operator will fail).
 * pstate should be provided if resolveUnknown is TRUE, but can be NULL
 * otherwise.
 *
 * Returns the updated SortGroupClause list.
 */
List *
addTargetToSortList(ParseState *pstate, TargetEntry *tle,
					List *sortlist, List *targetlist, SortBy *sortby,
					bool resolveUnknown)
{
	Oid			restype = exprType((Node *) tle->expr);
	Oid			sortop;
	Oid			eqop;
	bool		hashable;
	bool		reverse;
	int			location;
	ParseCallbackState pcbstate;

	/* if tlist item is an UNKNOWN literal, change it to TEXT */
	if (restype == UNKNOWNOID && resolveUnknown)
	{
		tle->expr = (Expr *) coerce_type(pstate, (Node *) tle->expr,
										 restype, TEXTOID, -1,
										 COERCION_IMPLICIT,
										 COERCE_IMPLICIT_CAST,
										 -1);
		restype = TEXTOID;
	}

	/*
	 * Rather than clutter the API of get_sort_group_operators and the other
	 * functions we're about to use, make use of error context callback to
	 * mark any error reports with a parse position.  We point to the operator
	 * location if present, else to the expression being sorted.  (NB: use the
	 * original untransformed expression here; the TLE entry might well point
	 * at a duplicate expression in the regular SELECT list.)
	 */
	location = sortby->location;
	if (location < 0)
		location = exprLocation(sortby->node);
	setup_parser_errposition_callback(&pcbstate, pstate, location);

	/* determine the sortop, eqop, and directionality */
	switch (sortby->sortby_dir)
	{
		case SORTBY_DEFAULT:
		case SORTBY_ASC:
			get_sort_group_operators(restype,
									 true, true, false,
									 &sortop, &eqop, NULL,
									 &hashable);
			reverse = false;
			break;
		case SORTBY_DESC:
			get_sort_group_operators(restype,
									 false, true, true,
									 NULL, &eqop, &sortop,
									 &hashable);
			reverse = true;
			break;
		case SORTBY_USING:
			Assert(sortby->useOp != NIL);
			sortop = compatible_oper_opid(sortby->useOp,
										  restype,
										  restype,
										  false);

			/*
			 * Verify it's a valid ordering operator, fetch the corresponding
			 * equality operator, and determine whether to consider it like
			 * ASC or DESC.
			 */
			eqop = get_equality_op_for_ordering_op(sortop, &reverse);
			if (!OidIsValid(eqop))
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					   errmsg("operator %s is not a valid ordering operator",
							  strVal(llast(sortby->useOp))),
						 errhint("Ordering operators must be \"<\" or \">\" members of btree operator families.")));

			/*
			 * Also see if the equality operator is hashable.
			 */
			hashable = op_hashjoinable(eqop, restype);
			break;
		default:
			elog(ERROR, "unrecognized sortby_dir: %d", sortby->sortby_dir);
			sortop = InvalidOid;	/* keep compiler quiet */
			eqop = InvalidOid;
			hashable = false;
			reverse = false;
			break;
	}

	cancel_parser_errposition_callback(&pcbstate);

	/* avoid making duplicate sortlist entries */
	if (!targetIsInSortList(tle, sortop, sortlist))
	{
		SortGroupClause *sortcl = makeNode(SortGroupClause);

		sortcl->tleSortGroupRef = assignSortGroupRef(tle, targetlist);

		sortcl->eqop = eqop;
		sortcl->sortop = sortop;
		sortcl->hashable = hashable;

		switch (sortby->sortby_nulls)
		{
			case SORTBY_NULLS_DEFAULT:
				/* NULLS FIRST is default for DESC; other way for ASC */
				sortcl->nulls_first = reverse;
				break;
			case SORTBY_NULLS_FIRST:
				sortcl->nulls_first = true;
				break;
			case SORTBY_NULLS_LAST:
				sortcl->nulls_first = false;
				break;
			default:
				elog(ERROR, "unrecognized sortby_nulls: %d",
					 sortby->sortby_nulls);
				break;
		}

		sortlist = lappend(sortlist, sortcl);
	}

	return sortlist;
}

/*
 * addTargetToGroupList
 *		If the given targetlist entry isn't already in the SortGroupClause
 *		list, add it to the end of the list, using default sort/group
 *		semantics.
 *
 * This is very similar to addTargetToSortList, except that we allow the
 * case where only a grouping (equality) operator can be found, and that
 * the TLE is considered "already in the list" if it appears there with any
 * sorting semantics.
 *
 * location is the parse location to be fingered in event of trouble.  Note
 * that we can't rely on exprLocation(tle->expr), because that might point
 * to a SELECT item that matches the GROUP BY item; it'd be pretty confusing
 * to report such a location.
 *
 * If resolveUnknown is TRUE, convert TLEs of type UNKNOWN to TEXT.  If not,
 * do nothing (which implies the search for an equality operator will fail).
 * pstate should be provided if resolveUnknown is TRUE, but can be NULL
 * otherwise.
 *
 * Returns the updated SortGroupClause list.
 */
static List *
addTargetToGroupList(ParseState *pstate, TargetEntry *tle,
					 List *grouplist, List *targetlist, int location,
					 bool resolveUnknown)
{
	Oid			restype = exprType((Node *) tle->expr);

	/* if tlist item is an UNKNOWN literal, change it to TEXT */
	if (restype == UNKNOWNOID && resolveUnknown)
	{
		tle->expr = (Expr *) coerce_type(pstate, (Node *) tle->expr,
										 restype, TEXTOID, -1,
										 COERCION_IMPLICIT,
										 COERCE_IMPLICIT_CAST,
										 -1);
		restype = TEXTOID;
	}

	/* avoid making duplicate grouplist entries */
	if (!targetIsInSortList(tle, InvalidOid, grouplist))
	{
		SortGroupClause *grpcl = makeNode(SortGroupClause);
		Oid			sortop;
		Oid			eqop;
		bool		hashable;
		ParseCallbackState pcbstate;

		setup_parser_errposition_callback(&pcbstate, pstate, location);

		/* determine the eqop and optional sortop */
		get_sort_group_operators(restype,
								 false, true, false,
								 &sortop, &eqop, NULL,
								 &hashable);

		cancel_parser_errposition_callback(&pcbstate);

		grpcl->tleSortGroupRef = assignSortGroupRef(tle, targetlist);
		grpcl->eqop = eqop;
		grpcl->sortop = sortop;
		grpcl->nulls_first = false;		/* OK with or without sortop */
		grpcl->hashable = hashable;

		grouplist = lappend(grouplist, grpcl);
	}

	return grouplist;
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
 *		If sortop is not InvalidOid, also test for a match to the sortop.
 *
 * It is not an oversight that this function ignores the nulls_first flag.
 * We check sortop when determining if an ORDER BY item is redundant with
 * earlier ORDER BY items, because it's conceivable that "ORDER BY
 * foo USING <, foo USING <<<" is not redundant, if <<< distinguishes
 * values that < considers equal.  We need not check nulls_first
 * however, because a lower-order column with the same sortop but
 * opposite nulls direction is redundant.  Also, we can consider
 * ORDER BY foo ASC, foo DESC redundant, so check for a commutator match.
 *
 * Works for both ordering and grouping lists (sortop would normally be
 * InvalidOid when considering grouping).  Note that the main reason we need
 * this routine (and not just a quick test for nonzeroness of ressortgroupref)
 * is that a TLE might be in only one of the lists.
 */
bool
targetIsInSortList(TargetEntry *tle, Oid sortop, List *sortList)
{
	Index		ref = tle->ressortgroupref;
	ListCell   *l;

	/* no need to scan list if tle has no marker */
	if (ref == 0)
		return false;

	foreach(l, sortList)
	{
		SortGroupClause *scl = (SortGroupClause *) lfirst(l);

		if (scl->tleSortGroupRef == ref &&
			(sortop == InvalidOid ||
			 sortop == scl->sortop ||
			 sortop == get_commutator(scl->sortop)))
			return true;
	}
	return false;
}

/*
 * findWindowClause
 *		Find the named WindowClause in the list, or return NULL if not there
 */
static WindowClause *
findWindowClause(List *wclist, const char *name)
{
	ListCell   *l;

	foreach(l, wclist)
	{
		WindowClause *wc = (WindowClause *) lfirst(l);

		if (wc->name && strcmp(wc->name, name) == 0)
			return wc;
	}

	return NULL;
}

/*
 * transformFrameOffset
 *		Process a window frame offset expression
 */
static Node *
transformFrameOffset(ParseState *pstate, int frameOptions, Node *clause)
{
	const char *constructName = NULL;
	Node	   *node;

	/* Quick exit if no offset expression */
	if (clause == NULL)
		return NULL;

	if (frameOptions & FRAMEOPTION_ROWS)
	{
		/* Transform the raw expression tree */
		node = transformExpr(pstate, clause, EXPR_KIND_WINDOW_FRAME_ROWS);

		/*
		 * Like LIMIT clause, simply coerce to int8
		 */
		constructName = "ROWS";
		node = coerce_to_specific_type(pstate, node, INT8OID, constructName);
	}
	else if (frameOptions & FRAMEOPTION_RANGE)
	{
		/* Transform the raw expression tree */
		node = transformExpr(pstate, clause, EXPR_KIND_WINDOW_FRAME_RANGE);

		/*
		 * this needs a lot of thought to decide how to support in the context
		 * of Postgres' extensible datatype framework
		 */
		constructName = "RANGE";
		/* error was already thrown by gram.y, this is just a backstop */
		elog(ERROR, "window frame with value offset is not implemented");
	}
	else
	{
		Assert(false);
		node = NULL;
	}

	/* Disallow variables in frame offsets */
	checkExprIsVarFree(pstate, node, constructName);

	return node;
}
