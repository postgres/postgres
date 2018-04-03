/*-------------------------------------------------------------------------
 *
 * parse_merge.c
 *	  handle merge-statement in parser
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_merge.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"

#include "access/sysattr.h"
#include "nodes/makefuncs.h"
#include "parser/analyze.h"
#include "parser/parse_collate.h"
#include "parser/parsetree.h"
#include "parser/parser.h"
#include "parser/parse_clause.h"
#include "parser/parse_cte.h"
#include "parser/parse_merge.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "utils/rel.h"
#include "utils/relcache.h"

static int transformMergeJoinClause(ParseState *pstate, Node *merge,
						List **mergeSourceTargetList);
static void setNamespaceForMergeAction(ParseState *pstate,
						MergeAction *action);
static void setNamespaceVisibilityForRTE(List *namespace, RangeTblEntry *rte,
							 bool rel_visible,
							 bool cols_visible);
static List *expandSourceTL(ParseState *pstate, RangeTblEntry *rte,
							int rtindex);

/*
 *	Special handling for MERGE statement is required because we assemble
 *	the query manually. This is similar to setTargetTable() followed
 * 	by transformFromClause() but with a few less steps.
 *
 *	Process the FROM clause and add items to the query's range table,
 *	joinlist, and namespace.
 *
 *	A special targetlist comprising of the columns from the right-subtree of
 *	the join is populated and returned. Note that when the JoinExpr is
 *	setup by transformMergeStmt, the left subtree has the target result
 *	relation and the right subtree has the source relation.
 *
 *	Returns the rangetable index of the target relation.
 */
static int
transformMergeJoinClause(ParseState *pstate, Node *merge,
						 List **mergeSourceTargetList)
{
	RangeTblEntry *rte,
			   *rt_rte;
	List	   *namespace;
	int			rtindex,
				rt_rtindex;
	Node	   *n;
	int			mergeTarget_relation = list_length(pstate->p_rtable) + 1;
	Var		   *var;
	TargetEntry *te;

	n = transformFromClauseItem(pstate, merge,
								&rte,
								&rtindex,
								&rt_rte,
								&rt_rtindex,
								&namespace);

	pstate->p_joinlist = list_make1(n);

	/*
	 * We created an internal join between the target and the source relation
	 * to carry out the MERGE actions. Normally such an unaliased join hides
	 * the joining relations, unless the column references are qualified.
	 * Also, any unqualified column references are resolved to the Join RTE, if
	 * there is a matching entry in the targetlist. But the way MERGE
	 * execution is later setup, we expect all column references to resolve to
	 * either the source or the target relation. Hence we must not add the
	 * Join RTE to the namespace.
	 *
	 * The last entry must be for the top-level Join RTE. We don't want to
	 * resolve any references to the Join RTE. So discard that.
	 *
	 * We also do not want to resolve any references from the leftside of the
	 * Join since that corresponds to the target relation. References to the
	 * columns of the target relation must be resolved from the result
	 * relation and not the one that is used in the join. So the
	 * mergeTarget_relation is marked invisible to both qualified as well as
	 * unqualified references.
	 */
	Assert(list_length(namespace) > 1);
	namespace = list_truncate(namespace, list_length(namespace) - 1);
	pstate->p_namespace = list_concat(pstate->p_namespace, namespace);

	setNamespaceVisibilityForRTE(pstate->p_namespace,
								 rt_fetch(mergeTarget_relation, pstate->p_rtable), false, false);

	/*
	 * Expand the right relation and add its columns to the
	 * mergeSourceTargetList. Note that the right relation can either be a
	 * plain relation or a subquery or anything that can have a
	 * RangeTableEntry.
	 */
	*mergeSourceTargetList = expandSourceTL(pstate, rt_rte, rt_rtindex);

	/*
	 * Add a whole-row-Var entry to support references to "source.*".
	 */
	var = makeWholeRowVar(rt_rte, rt_rtindex, 0, false);
	te = makeTargetEntry((Expr *) var, list_length(*mergeSourceTargetList) + 1,
						 NULL, true);
	*mergeSourceTargetList = lappend(*mergeSourceTargetList, te);

	return mergeTarget_relation;
}

/*
 * Make appropriate changes to the namespace visibility while transforming
 * individual action's quals and targetlist expressions. In particular, for
 * INSERT actions we must only see the source relation (since INSERT action is
 * invoked for NOT MATCHED tuples and hence there is no target tuple to deal
 * with). On the other hand, UPDATE and DELETE actions can see both source and
 * target relations.
 *
 * Also, since the internal Join node can hide the source and target
 * relations, we must explicitly make the respective relation as visible so
 * that columns can be referenced unqualified from these relations.
 */
static void
setNamespaceForMergeAction(ParseState *pstate, MergeAction *action)
{
	RangeTblEntry *targetRelRTE,
			   *sourceRelRTE;

	/* Assume target relation is at index 1 */
	targetRelRTE = rt_fetch(1, pstate->p_rtable);

	/*
	 * Assume that the top-level join RTE is at the end. The source relation
	 * is just before that.
	 */
	sourceRelRTE = rt_fetch(list_length(pstate->p_rtable) - 1, pstate->p_rtable);

	switch (action->commandType)
	{
		case CMD_INSERT:

			/*
			 * Inserts can't see target relation, but they can see source
			 * relation.
			 */
			setNamespaceVisibilityForRTE(pstate->p_namespace,
										 targetRelRTE, false, false);
			setNamespaceVisibilityForRTE(pstate->p_namespace,
										 sourceRelRTE, true, true);
			break;

		case CMD_UPDATE:
		case CMD_DELETE:

			/*
			 * Updates and deletes can see both target and source relations.
			 */
			setNamespaceVisibilityForRTE(pstate->p_namespace,
										 targetRelRTE, true, true);
			setNamespaceVisibilityForRTE(pstate->p_namespace,
										 sourceRelRTE, true, true);
			break;

		case CMD_NOTHING:
			break;
		default:
			elog(ERROR, "unknown action in MERGE WHEN clause");
	}
}

/*
 * transformMergeStmt -
 *	  transforms a MERGE statement
 */
Query *
transformMergeStmt(ParseState *pstate, MergeStmt *stmt)
{
	Query		   *qry = makeNode(Query);
	ListCell	   *l;
	AclMode			targetPerms = ACL_NO_RIGHTS;
	bool			is_terminal[2];
	JoinExpr	   *joinexpr;
	RangeTblEntry  *resultRelRTE, *mergeRelRTE;

	/* There can't be any outer WITH to worry about */
	Assert(pstate->p_ctenamespace == NIL);

	qry->commandType = CMD_MERGE;
	qry->hasRecursive = false;

	/* process the WITH clause independently of all else */
	if (stmt->withClause)
	{
		if (stmt->withClause->recursive)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("WITH RECURSIVE is not supported for MERGE statement")));

		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/*
	 * Check WHEN clauses for permissions and sanity
	 */
	is_terminal[0] = false;
	is_terminal[1] = false;
	foreach(l, stmt->mergeActionList)
	{
		MergeAction *action = (MergeAction *) lfirst(l);
		int		when_type = (action->matched ? 0 : 1);

		/*
		 * Collect action types so we can check Target permissions
		 */
		switch (action->commandType)
		{
			case CMD_INSERT:
				{
					InsertStmt *istmt = (InsertStmt *) action->stmt;
					SelectStmt *selectStmt = (SelectStmt *) istmt->selectStmt;

					/*
					 * The grammar allows attaching ORDER BY, LIMIT, FOR
					 * UPDATE, or WITH to a VALUES clause and also multiple
					 * VALUES clauses. If we have any of those, ERROR.
					 */
					if (selectStmt && (selectStmt->valuesLists == NIL ||
									   selectStmt->sortClause != NIL ||
									   selectStmt->limitOffset != NULL ||
									   selectStmt->limitCount != NULL ||
									   selectStmt->lockingClause != NIL ||
									   selectStmt->withClause != NULL))
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("SELECT not allowed in MERGE INSERT statement")));

					if (selectStmt && list_length(selectStmt->valuesLists) > 1)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("Multiple VALUES clauses not allowed in MERGE INSERT statement")));

					targetPerms |= ACL_INSERT;
				}
				break;
			case CMD_UPDATE:
				targetPerms |= ACL_UPDATE;
				break;
			case CMD_DELETE:
				targetPerms |= ACL_DELETE;
				break;
			case CMD_NOTHING:
				break;
			default:
				elog(ERROR, "unknown action in MERGE WHEN clause");
		}

		/*
		 * Check for unreachable WHEN clauses
		 */
		if (action->condition == NULL)
			is_terminal[when_type] = true;
		else if (is_terminal[when_type])
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unreachable WHEN clause specified after unconditional WHEN clause")));
	}

	/*
	 * Construct a query of the form
	 * 	SELECT relation.ctid	--junk attribute
	 *		  ,relation.tableoid	--junk attribute
	 * 		  ,source_relation.<somecols>
	 * 		  ,relation.<somecols>
	 *  FROM relation RIGHT JOIN source_relation
	 *  ON  join_condition; -- no WHERE clause - all conditions are applied in
	 * executor
	 *
	 * stmt->relation is the target relation, given as a RangeVar
	 * stmt->source_relation is a RangeVar or subquery
	 *
	 * We specify the join as a RIGHT JOIN as a simple way of forcing the
	 * first (larg) RTE to refer to the target table.
	 *
	 * The MERGE query's join can be tuned in some cases, see below for these
	 * special case tweaks.
	 *
	 * We set QSRC_PARSER to show query constructed in parse analysis
	 *
	 * Note that we have only one Query for a MERGE statement and the planner
	 * is called only once. That query is executed once to produce our stream
	 * of candidate change rows, so the query must contain all of the columns
	 * required by each of the targetlist or conditions for each action.
	 *
	 * As top-level statements INSERT, UPDATE and DELETE have a Query, whereas
	 * with MERGE the individual actions do not require separate planning,
	 * only different handling in the executor. See nodeModifyTable handling
	 * of commandType CMD_MERGE.
	 *
	 * A sub-query can include the Target, but otherwise the sub-query cannot
	 * reference the outermost Target table at all.
	 */
	qry->querySource = QSRC_PARSER;

	/*
	 * Setup the target table. Unlike regular UPDATE/DELETE, we don't expand
	 * inheritance for the target relation in case of MERGE.
	 *
	 * This special arrangement is required for handling partitioned tables
	 * because we perform an JOIN between the target and the source relation to
	 * identify the matching and not-matching rows. If we take the usual path
	 * of expanding the target table's inheritance and create one subplan per
	 * partition, then we we won't be able to correctly identify the matching
	 * and not-matching rows since for a given source row, there may not be a
	 * matching row in one partition, but it may exists in some other
	 * partition. So we must first append all the qualifying rows from all the
	 * partitions and then do the matching.
	 *
	 * Once a target row is returned by the underlying join, we find the
	 * correct partition and setup required state to carry out UPDATE/DELETE.
	 * All of this happens during execution.
	 */
	qry->resultRelation = setTargetTable(pstate, stmt->relation,
										 false,	/* do not expand inheritance */
										 true, targetPerms);

	/*
	 * Create a JOIN between the target and the source relation.
	 */
	joinexpr = makeNode(JoinExpr);
	joinexpr->isNatural = false;
	joinexpr->alias = NULL;
	joinexpr->usingClause = NIL;
	joinexpr->quals = stmt->join_condition;
	joinexpr->larg = (Node *) stmt->relation;
	joinexpr->rarg = (Node *) stmt->source_relation;

	/*
	 * Simplify the MERGE query as much as possible
	 *
	 * These seem like things that could go into Optimizer, but they are
	 * semantic simplifications rather than optimizations, per se.
	 *
	 * If there are no INSERT actions we won't be using the non-matching
	 * candidate rows for anything, so no need for an outer join. We do still
	 * need an inner join for UPDATE and DELETE actions.
	 */
	if (targetPerms & ACL_INSERT)
		joinexpr->jointype = JOIN_RIGHT;
	else
		joinexpr->jointype = JOIN_INNER;

	/*
	 * We use a special purpose transformation here because the normal
	 * routines don't quite work right for the MERGE case.
	 *
	 * A special mergeSourceTargetList is setup by transformMergeJoinClause().
	 * It refers to all the attributes provided by the source relation. This
	 * is later used by set_plan_refs() to fix the UPDATE/INSERT target lists
	 * to so that they can correctly fetch the attributes from the source
	 * relation.
	 *
	 * The target relation when used in the underlying join, gets a new RTE
	 * with rte->inh set to true. We remember this RTE (and later pass on to
	 * the planner and executor) for two main reasons:
	 *
	 * 1. If we ever need to run EvalPlanQual while performing MERGE, we must
	 * make the modified tuple available to the underlying join query, which is
	 * using a different RTE from the resultRelation RTE.
	 *
	 * 2. rewriteTargetListMerge() requires the RTE of the underlying join in
	 * order to add junk CTID and TABLEOID attributes.
	 */
	qry->mergeTarget_relation = transformMergeJoinClause(pstate, (Node *) joinexpr,
														 &qry->mergeSourceTargetList);

	/*
	 * The target table referenced in the MERGE is looked up twice; once while
	 * setting it up as the result relation and again when it's used in the
	 * underlying the join query. In some rare situations, it may happen that
	 * these lookups return different results, for example, if a new relation
	 * with the same name gets created in a schema which is ahead in the
	 * search_path, in between the two lookups.
	 *
	 * It's a very narrow case, but nevertheless we guard against it by simply
	 * checking if the OIDs returned by the two lookups is the same. If not, we
	 * just throw an error.
	 */
	Assert(qry->resultRelation > 0);
	Assert(qry->mergeTarget_relation > 0);

	/* Fetch both the RTEs */
	resultRelRTE = rt_fetch(qry->resultRelation, pstate->p_rtable);
	mergeRelRTE = rt_fetch(qry->mergeTarget_relation, pstate->p_rtable);

	if (resultRelRTE->relid != mergeRelRTE->relid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("relation referenced by MERGE statement has changed")));

	/*
	 * This query should just provide the source relation columns. Later, in
	 * preprocess_targetlist(), we shall also add "ctid" attribute of the
	 * target relation to ensure that the target tuple can be fetched
	 * correctly.
	 */
	qry->targetList = qry->mergeSourceTargetList;

	/* qry has no WHERE clause so absent quals are shown as NULL */
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);
	qry->rtable = pstate->p_rtable;

	/*
	 * XXX MERGE is unsupported in various cases
	 */
	if (!(pstate->p_target_relation->rd_rel->relkind == RELKIND_RELATION ||
		  pstate->p_target_relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("MERGE is not supported for this relation type")));

	if (pstate->p_target_relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE &&
		pstate->p_target_relation->rd_rel->relhassubclass)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("MERGE is not supported for relations with inheritance")));

	if (pstate->p_target_relation->rd_rel->relhasrules)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("MERGE is not supported for relations with rules")));

	/*
	 * We now have a good query shape, so now look at the when conditions and
	 * action targetlists.
	 *
	 * Overall, the MERGE Query's targetlist is NIL.
	 *
	 * Each individual action has its own targetlist that needs separate
	 * transformation. These transforms don't do anything to the overall
	 * targetlist, since that is only used for resjunk columns.
	 *
	 * We can reference any column in Target or Source, which is OK because
	 * both of those already have RTEs. There is nothing like the EXCLUDED
	 * pseudo-relation for INSERT ON CONFLICT.
	 */
	foreach(l, stmt->mergeActionList)
	{
		MergeAction *action = (MergeAction *) lfirst(l);

		/*
		 * Set namespace for the specific action. This must be done before
		 * analyzing the WHEN quals and the action targetlisst.
		 */
		setNamespaceForMergeAction(pstate, action);

		/*
		 * Transform the when condition.
		 *
		 * Note that these quals are NOT added to the join quals; instead they
		 * are evaluated separately during execution to decide which of the
		 * WHEN MATCHED or WHEN NOT MATCHED actions to execute.
		 */
		action->qual = transformWhereClause(pstate, action->condition,
											EXPR_KIND_MERGE_WHEN_AND, "WHEN");

		/*
		 * Transform target lists for each INSERT and UPDATE action stmt
		 */
		switch (action->commandType)
		{
			case CMD_INSERT:
				{
					InsertStmt *istmt = (InsertStmt *) action->stmt;
					SelectStmt *selectStmt = (SelectStmt *) istmt->selectStmt;
					List	   *exprList = NIL;
					ListCell   *lc;
					RangeTblEntry *rte;
					ListCell   *icols;
					ListCell   *attnos;
					List	   *icolumns;
					List	   *attrnos;

					pstate->p_is_insert = true;

					icolumns = checkInsertTargets(pstate, istmt->cols, &attrnos);
					Assert(list_length(icolumns) == list_length(attrnos));

					/*
					 * Handle INSERT much like in transformInsertStmt
					 */
					if (selectStmt == NULL)
					{
						/*
						 * We have INSERT ... DEFAULT VALUES.  We can handle
						 * this case by emitting an empty targetlist --- all
						 * columns will be defaulted when the planner expands
						 * the targetlist.
						 */
						exprList = NIL;
					}
					else
					{
						/*
						 * Process INSERT ... VALUES with a single VALUES
						 * sublist.  We treat this case separately for
						 * efficiency.  The sublist is just computed directly
						 * as the Query's targetlist, with no VALUES RTE.  So
						 * it works just like a SELECT without any FROM.
						 */
						List	   *valuesLists = selectStmt->valuesLists;

						Assert(list_length(valuesLists) == 1);
						Assert(selectStmt->intoClause == NULL);

						/*
						 * Do basic expression transformation (same as a ROW()
						 * expr, but allow SetToDefault at top level)
						 */
						exprList = transformExpressionList(pstate,
														   (List *) linitial(valuesLists),
														   EXPR_KIND_VALUES_SINGLE,
														   true);

						/* Prepare row for assignment to target table */
						exprList = transformInsertRow(pstate, exprList,
													  istmt->cols,
													  icolumns, attrnos,
													  false);
					}

					/*
					 * Generate action's target list using the computed list
					 * of expressions. Also, mark all the target columns as
					 * needing insert permissions.
					 */
					rte = pstate->p_target_rangetblentry;
					icols = list_head(icolumns);
					attnos = list_head(attrnos);
					foreach(lc, exprList)
					{
						Expr	   *expr = (Expr *) lfirst(lc);
						ResTarget  *col;
						AttrNumber	attr_num;
						TargetEntry *tle;

						col = lfirst_node(ResTarget, icols);
						attr_num = (AttrNumber) lfirst_int(attnos);

						tle = makeTargetEntry(expr,
											  attr_num,
											  col->name,
											  false);
						action->targetList = lappend(action->targetList, tle);

						rte->insertedCols = bms_add_member(rte->insertedCols,
														   attr_num - FirstLowInvalidHeapAttributeNumber);

						icols = lnext(icols);
						attnos = lnext(attnos);
					}
				}
				break;
			case CMD_UPDATE:
				{
					UpdateStmt *ustmt = (UpdateStmt *) action->stmt;

					pstate->p_is_insert = false;
					action->targetList = transformUpdateTargetList(pstate, ustmt->targetList);
				}
				break;
			case CMD_DELETE:
				break;

			case CMD_NOTHING:
				action->targetList = NIL;
				break;
			default:
				elog(ERROR, "unknown action in MERGE WHEN clause");
		}
	}

	qry->mergeActionList = stmt->mergeActionList;

	/* XXX maybe later */
	qry->returningList = NULL;

	qry->hasTargetSRFs = false;
	qry->hasSubLinks = pstate->p_hasSubLinks;

	assign_query_collations(pstate, qry);

	return qry;
}

static void
setNamespaceVisibilityForRTE(List *namespace, RangeTblEntry *rte,
							 bool rel_visible,
							 bool cols_visible)
{
	ListCell   *lc;

	foreach(lc, namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(lc);

		if (nsitem->p_rte == rte)
		{
			nsitem->p_rel_visible = rel_visible;
			nsitem->p_cols_visible = cols_visible;
			break;
		}
	}

}

/*
 * Expand the source relation to include all attributes of this RTE.
 *
 * This function is very similar to expandRelAttrs except that we don't mark
 * columns for SELECT privileges. That will be decided later when we transform
 * the action targetlists and the WHEN quals for actual references to the
 * source relation.
 */
static List *
expandSourceTL(ParseState *pstate, RangeTblEntry *rte, int rtindex)
{
	List	   *names,
			   *vars;
	ListCell   *name,
			   *var;
	List	   *te_list = NIL;

	expandRTE(rte, rtindex, 0, -1, false, &names, &vars);

	/*
	 * Require read access to the table.
	 */
	rte->requiredPerms |= ACL_SELECT;

	forboth(name, names, var, vars)
	{
		char	   *label = strVal(lfirst(name));
		Var		   *varnode = (Var *) lfirst(var);
		TargetEntry *te;

		te = makeTargetEntry((Expr *) varnode,
							 (AttrNumber) pstate->p_next_resno++,
							 label,
							 false);
		te_list = lappend(te_list, te);
	}

	Assert(name == NULL && var == NULL);	/* lists not the same length? */

	return te_list;
}
