/*-------------------------------------------------------------------------
 *
 * parse_merge.c
 *	  handle merge-statement in parser
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_merge.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/sysattr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/analyze.h"
#include "parser/parse_collate.h"
#include "parser/parsetree.h"
#include "parser/parser.h"
#include "parser/parse_clause.h"
#include "parser/parse_cte.h"
#include "parser/parse_expr.h"
#include "parser/parse_merge.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "utils/rel.h"
#include "utils/relcache.h"

static void setNamespaceForMergeWhen(ParseState *pstate,
									 MergeWhenClause *mergeWhenClause,
									 Index targetRTI,
									 Index sourceRTI);
static void setNamespaceVisibilityForRTE(List *namespace, RangeTblEntry *rte,
										 bool rel_visible,
										 bool cols_visible);

/*
 * Make appropriate changes to the namespace visibility while transforming
 * individual action's quals and targetlist expressions. In particular, for
 * INSERT actions we must only see the source relation (since INSERT action is
 * invoked for NOT MATCHED tuples and hence there is no target tuple to deal
 * with). On the other hand, UPDATE and DELETE actions can see both source and
 * target relations.
 *
 * Also, since the internal join node can hide the source and target
 * relations, we must explicitly make the respective relation as visible so
 * that columns can be referenced unqualified from these relations.
 */
static void
setNamespaceForMergeWhen(ParseState *pstate, MergeWhenClause *mergeWhenClause,
						 Index targetRTI, Index sourceRTI)
{
	RangeTblEntry *targetRelRTE,
			   *sourceRelRTE;

	targetRelRTE = rt_fetch(targetRTI, pstate->p_rtable);
	sourceRelRTE = rt_fetch(sourceRTI, pstate->p_rtable);

	if (mergeWhenClause->matched)
	{
		Assert(mergeWhenClause->commandType == CMD_UPDATE ||
			   mergeWhenClause->commandType == CMD_DELETE ||
			   mergeWhenClause->commandType == CMD_NOTHING);

		/* MATCHED actions can see both target and source relations. */
		setNamespaceVisibilityForRTE(pstate->p_namespace,
									 targetRelRTE, true, true);
		setNamespaceVisibilityForRTE(pstate->p_namespace,
									 sourceRelRTE, true, true);
	}
	else
	{
		/*
		 * NOT MATCHED actions can't see target relation, but they can see
		 * source relation.
		 */
		Assert(mergeWhenClause->commandType == CMD_INSERT ||
			   mergeWhenClause->commandType == CMD_NOTHING);
		setNamespaceVisibilityForRTE(pstate->p_namespace,
									 targetRelRTE, false, false);
		setNamespaceVisibilityForRTE(pstate->p_namespace,
									 sourceRelRTE, true, true);
	}
}

/*
 * transformMergeStmt -
 *	  transforms a MERGE statement
 */
Query *
transformMergeStmt(ParseState *pstate, MergeStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	ListCell   *l;
	AclMode		targetPerms = ACL_NO_RIGHTS;
	bool		is_terminal[2];
	Index		sourceRTI;
	List	   *mergeActionList;
	Node	   *joinExpr;
	ParseNamespaceItem *nsitem;

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
	foreach(l, stmt->mergeWhenClauses)
	{
		MergeWhenClause *mergeWhenClause = (MergeWhenClause *) lfirst(l);
		int			when_type = (mergeWhenClause->matched ? 0 : 1);

		/*
		 * Collect action types so we can check target permissions
		 */
		switch (mergeWhenClause->commandType)
		{
			case CMD_INSERT:
				targetPerms |= ACL_INSERT;
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
		if (mergeWhenClause->condition == NULL)
			is_terminal[when_type] = true;
		else if (is_terminal[when_type])
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unreachable WHEN clause specified after unconditional WHEN clause")));
	}

	/* Set up the MERGE target table. */
	qry->resultRelation = setTargetTable(pstate, stmt->relation,
										 stmt->relation->inh,
										 false, targetPerms);

	/*
	 * MERGE is unsupported in various cases
	 */
	if (pstate->p_target_relation->rd_rel->relkind != RELKIND_RELATION &&
		pstate->p_target_relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot execute MERGE on relation \"%s\"",
						RelationGetRelationName(pstate->p_target_relation)),
				 errdetail_relkind_not_supported(pstate->p_target_relation->rd_rel->relkind)));
	if (pstate->p_target_relation->rd_rel->relhasrules)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot execute MERGE on relation \"%s\"",
						RelationGetRelationName(pstate->p_target_relation)),
				 errdetail("MERGE is not supported for relations with rules.")));

	/* Now transform the source relation to produce the source RTE. */
	transformFromClause(pstate,
						list_make1(stmt->sourceRelation));
	sourceRTI = list_length(pstate->p_rtable);
	nsitem = GetNSItemByRangeTablePosn(pstate, sourceRTI, 0);

	/*
	 * Check that the target table doesn't conflict with the source table.
	 * This would typically be a checkNameSpaceConflicts call, but we want a
	 * more specific error message.
	 */
	if (strcmp(pstate->p_target_nsitem->p_names->aliasname,
			   nsitem->p_names->aliasname) == 0)
		ereport(ERROR,
				errcode(ERRCODE_DUPLICATE_ALIAS),
				errmsg("name \"%s\" specified more than once",
					   pstate->p_target_nsitem->p_names->aliasname),
				errdetail("The name is used both as MERGE target table and data source."));

	/*
	 * There's no need for a targetlist here; it'll be set up by
	 * preprocess_targetlist later.
	 */
	qry->targetList = NIL;
	qry->rtable = pstate->p_rtable;

	/*
	 * Transform the join condition.  This includes references to the target
	 * side, so add that to the namespace.
	 */
	addNSItemToQuery(pstate, pstate->p_target_nsitem, false, true, true);
	joinExpr = transformExpr(pstate, stmt->joinCondition,
							 EXPR_KIND_JOIN_ON);

	/*
	 * Create the temporary query's jointree using the joinlist we built using
	 * just the source relation; the target relation is not included.  The
	 * quals we use are the join conditions to the merge target.  The join
	 * will be constructed fully by transform_MERGE_to_join.
	 */
	qry->jointree = makeFromExpr(pstate->p_joinlist, joinExpr);

	/*
	 * We now have a good query shape, so now look at the WHEN conditions and
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
	mergeActionList = NIL;
	foreach(l, stmt->mergeWhenClauses)
	{
		MergeWhenClause *mergeWhenClause = lfirst_node(MergeWhenClause, l);
		MergeAction *action;

		action = makeNode(MergeAction);
		action->commandType = mergeWhenClause->commandType;
		action->matched = mergeWhenClause->matched;

		/* Use an outer join if any INSERT actions exist in the command. */
		if (action->commandType == CMD_INSERT)
			qry->mergeUseOuterJoin = true;

		/*
		 * Set namespace for the specific action. This must be done before
		 * analyzing the WHEN quals and the action targetlist.
		 */
		setNamespaceForMergeWhen(pstate, mergeWhenClause,
								 qry->resultRelation,
								 sourceRTI);

		/*
		 * Transform the WHEN condition.
		 *
		 * Note that these quals are NOT added to the join quals; instead they
		 * are evaluated separately during execution to decide which of the
		 * WHEN MATCHED or WHEN NOT MATCHED actions to execute.
		 */
		action->qual = transformWhereClause(pstate, mergeWhenClause->condition,
											EXPR_KIND_MERGE_WHEN, "WHEN");

		/*
		 * Transform target lists for each INSERT and UPDATE action stmt
		 */
		switch (action->commandType)
		{
			case CMD_INSERT:
				{
					List	   *exprList = NIL;
					ListCell   *lc;
					RangeTblEntry *rte;
					ListCell   *icols;
					ListCell   *attnos;
					List	   *icolumns;
					List	   *attrnos;

					pstate->p_is_insert = true;

					icolumns = checkInsertTargets(pstate,
												  mergeWhenClause->targetList,
												  &attrnos);
					Assert(list_length(icolumns) == list_length(attrnos));

					action->override = mergeWhenClause->override;

					/*
					 * Handle INSERT much like in transformInsertStmt
					 */
					if (mergeWhenClause->values == NIL)
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

						/*
						 * Do basic expression transformation (same as a ROW()
						 * expr, but allow SetToDefault at top level)
						 */
						exprList = transformExpressionList(pstate,
														   mergeWhenClause->values,
														   EXPR_KIND_VALUES_SINGLE,
														   true);

						/* Prepare row for assignment to target table */
						exprList = transformInsertRow(pstate, exprList,
													  mergeWhenClause->targetList,
													  icolumns, attrnos,
													  false);
					}

					/*
					 * Generate action's target list using the computed list
					 * of expressions. Also, mark all the target columns as
					 * needing insert permissions.
					 */
					rte = pstate->p_target_nsitem->p_rte;
					forthree(lc, exprList, icols, icolumns, attnos, attrnos)
					{
						Expr	   *expr = (Expr *) lfirst(lc);
						ResTarget  *col = lfirst_node(ResTarget, icols);
						AttrNumber	attr_num = (AttrNumber) lfirst_int(attnos);
						TargetEntry *tle;

						tle = makeTargetEntry(expr,
											  attr_num,
											  col->name,
											  false);
						action->targetList = lappend(action->targetList, tle);

						rte->insertedCols =
							bms_add_member(rte->insertedCols,
										   attr_num - FirstLowInvalidHeapAttributeNumber);
					}
				}
				break;
			case CMD_UPDATE:
				{
					pstate->p_is_insert = false;
					action->targetList =
						transformUpdateTargetList(pstate,
												  mergeWhenClause->targetList);
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

		mergeActionList = lappend(mergeActionList, action);
	}

	qry->mergeActionList = mergeActionList;

	/* RETURNING could potentially be added in the future, but not in SQL std */
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
