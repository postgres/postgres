/*-------------------------------------------------------------------------
 *
 * rewriteHandler.c
 *		Primary module of query rewriter.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/rewrite/rewriteHandler.c
 *
 * NOTES
 *	  Some of the terms used in this file are of historic nature: "retrieve"
 *	  was the PostQUEL keyword for what today is SELECT. "RIR" stands for
 *	  "Retrieve-Instead-Retrieve", that is an ON SELECT DO INSTEAD SELECT rule
 *	  (which has to be unconditional and where only one rule can exist on each
 *	  relation).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_coerce.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rewriteSearchCycle.h"
#include "rewrite/rowsecurity.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"


/* We use a list of these to detect recursion in RewriteQuery */
typedef struct rewrite_event
{
	Oid			relation;		/* OID of relation having rules */
	CmdType		event;			/* type of rule being fired */
} rewrite_event;

typedef struct acquireLocksOnSubLinks_context
{
	bool		for_execute;	/* AcquireRewriteLocks' forExecute param */
} acquireLocksOnSubLinks_context;

typedef struct fireRIRonSubLink_context
{
	List	   *activeRIRs;
	bool		hasRowSecurity;
} fireRIRonSubLink_context;

static bool acquireLocksOnSubLinks(Node *node,
								   acquireLocksOnSubLinks_context *context);
static Query *rewriteRuleAction(Query *parsetree,
								Query *rule_action,
								Node *rule_qual,
								int rt_index,
								CmdType event,
								bool *returning_flag);
static List *adjustJoinTreeList(Query *parsetree, bool removert, int rt_index);
static List *rewriteTargetListIU(List *targetList,
								 CmdType commandType,
								 OverridingKind override,
								 Relation target_relation,
								 RangeTblEntry *values_rte,
								 int values_rte_index,
								 Bitmapset **unused_values_attrnos);
static TargetEntry *process_matched_tle(TargetEntry *src_tle,
										TargetEntry *prior_tle,
										const char *attrName);
static Node *get_assignment_input(Node *node);
static Bitmapset *findDefaultOnlyColumns(RangeTblEntry *rte);
static bool rewriteValuesRTE(Query *parsetree, RangeTblEntry *rte, int rti,
							 Relation target_relation,
							 Bitmapset *unused_cols);
static void rewriteValuesRTEToNulls(Query *parsetree, RangeTblEntry *rte);
static void markQueryForLocking(Query *qry, Node *jtnode,
								LockClauseStrength strength, LockWaitPolicy waitPolicy,
								bool pushedDown);
static List *matchLocks(CmdType event, Relation relation,
						int varno, Query *parsetree, bool *hasUpdate);
static Query *fireRIRrules(Query *parsetree, List *activeRIRs);
static Bitmapset *adjust_view_column_set(Bitmapset *cols, List *targetlist);
static Node *expand_generated_columns_internal(Node *node, Relation rel, int rt_index,
											   RangeTblEntry *rte, int result_relation);


/*
 * AcquireRewriteLocks -
 *	  Acquire suitable locks on all the relations mentioned in the Query.
 *	  These locks will ensure that the relation schemas don't change under us
 *	  while we are rewriting, planning, and executing the query.
 *
 * Caution: this may modify the querytree, therefore caller should usually
 * have done a copyObject() to make a writable copy of the querytree in the
 * current memory context.
 *
 * forExecute indicates that the query is about to be executed.  If so,
 * we'll acquire the lock modes specified in the RTE rellockmode fields.
 * If forExecute is false, AccessShareLock is acquired on all relations.
 * This case is suitable for ruleutils.c, for example, where we only need
 * schema stability and we don't intend to actually modify any relations.
 *
 * forUpdatePushedDown indicates that a pushed-down FOR [KEY] UPDATE/SHARE
 * applies to the current subquery, requiring all rels to be opened with at
 * least RowShareLock.  This should always be false at the top of the
 * recursion.  When it is true, we adjust RTE rellockmode fields to reflect
 * the higher lock level.  This flag is ignored if forExecute is false.
 *
 * A secondary purpose of this routine is to fix up JOIN RTE references to
 * dropped columns (see details below).  Such RTEs are modified in-place.
 *
 * This processing can, and for efficiency's sake should, be skipped when the
 * querytree has just been built by the parser: parse analysis already got
 * all the same locks we'd get here, and the parser will have omitted dropped
 * columns from JOINs to begin with.  But we must do this whenever we are
 * dealing with a querytree produced earlier than the current command.
 *
 * About JOINs and dropped columns: although the parser never includes an
 * already-dropped column in a JOIN RTE's alias var list, it is possible for
 * such a list in a stored rule to include references to dropped columns.
 * (If the column is not explicitly referenced anywhere else in the query,
 * the dependency mechanism won't consider it used by the rule and so won't
 * prevent the column drop.)  To support get_rte_attribute_is_dropped(), we
 * replace join alias vars that reference dropped columns with null pointers.
 *
 * (In PostgreSQL 8.0, we did not do this processing but instead had
 * get_rte_attribute_is_dropped() recurse to detect dropped columns in joins.
 * That approach had horrible performance unfortunately; in particular
 * construction of a nested join was O(N^2) in the nesting depth.)
 */
void
AcquireRewriteLocks(Query *parsetree,
					bool forExecute,
					bool forUpdatePushedDown)
{
	ListCell   *l;
	int			rt_index;
	acquireLocksOnSubLinks_context context;

	context.for_execute = forExecute;

	/*
	 * First, process RTEs of the current query level.
	 */
	rt_index = 0;
	foreach(l, parsetree->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);
		Relation	rel;
		LOCKMODE	lockmode;
		List	   *newaliasvars;
		Index		curinputvarno;
		RangeTblEntry *curinputrte;
		ListCell   *ll;

		++rt_index;
		switch (rte->rtekind)
		{
			case RTE_RELATION:

				/*
				 * Grab the appropriate lock type for the relation, and do not
				 * release it until end of transaction.  This protects the
				 * rewriter, planner, and executor against schema changes
				 * mid-query.
				 *
				 * If forExecute is false, ignore rellockmode and just use
				 * AccessShareLock.
				 */
				if (!forExecute)
					lockmode = AccessShareLock;
				else if (forUpdatePushedDown)
				{
					/* Upgrade RTE's lock mode to reflect pushed-down lock */
					if (rte->rellockmode == AccessShareLock)
						rte->rellockmode = RowShareLock;
					lockmode = rte->rellockmode;
				}
				else
					lockmode = rte->rellockmode;

				rel = table_open(rte->relid, lockmode);

				/*
				 * While we have the relation open, update the RTE's relkind,
				 * just in case it changed since this rule was made.
				 */
				rte->relkind = rel->rd_rel->relkind;

				table_close(rel, NoLock);
				break;

			case RTE_JOIN:

				/*
				 * Scan the join's alias var list to see if any columns have
				 * been dropped, and if so replace those Vars with null
				 * pointers.
				 *
				 * Since a join has only two inputs, we can expect to see
				 * multiple references to the same input RTE; optimize away
				 * multiple fetches.
				 */
				newaliasvars = NIL;
				curinputvarno = 0;
				curinputrte = NULL;
				foreach(ll, rte->joinaliasvars)
				{
					Var		   *aliasitem = (Var *) lfirst(ll);
					Var		   *aliasvar = aliasitem;

					/* Look through any implicit coercion */
					aliasvar = (Var *) strip_implicit_coercions((Node *) aliasvar);

					/*
					 * If the list item isn't a simple Var, then it must
					 * represent a merged column, ie a USING column, and so it
					 * couldn't possibly be dropped, since it's referenced in
					 * the join clause.  (Conceivably it could also be a null
					 * pointer already?  But that's OK too.)
					 */
					if (aliasvar && IsA(aliasvar, Var))
					{
						/*
						 * The elements of an alias list have to refer to
						 * earlier RTEs of the same rtable, because that's the
						 * order the planner builds things in.  So we already
						 * processed the referenced RTE, and so it's safe to
						 * use get_rte_attribute_is_dropped on it. (This might
						 * not hold after rewriting or planning, but it's OK
						 * to assume here.)
						 */
						Assert(aliasvar->varlevelsup == 0);
						if (aliasvar->varno != curinputvarno)
						{
							curinputvarno = aliasvar->varno;
							if (curinputvarno >= rt_index)
								elog(ERROR, "unexpected varno %d in JOIN RTE %d",
									 curinputvarno, rt_index);
							curinputrte = rt_fetch(curinputvarno,
												   parsetree->rtable);
						}
						if (get_rte_attribute_is_dropped(curinputrte,
														 aliasvar->varattno))
						{
							/* Replace the join alias item with a NULL */
							aliasitem = NULL;
						}
					}
					newaliasvars = lappend(newaliasvars, aliasitem);
				}
				rte->joinaliasvars = newaliasvars;
				break;

			case RTE_SUBQUERY:

				/*
				 * The subquery RTE itself is all right, but we have to
				 * recurse to process the represented subquery.
				 */
				AcquireRewriteLocks(rte->subquery,
									forExecute,
									(forUpdatePushedDown ||
									 get_parse_rowmark(parsetree, rt_index) != NULL));
				break;

			default:
				/* ignore other types of RTEs */
				break;
		}
	}

	/* Recurse into subqueries in WITH */
	foreach(l, parsetree->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(l);

		AcquireRewriteLocks((Query *) cte->ctequery, forExecute, false);
	}

	/*
	 * Recurse into sublink subqueries, too.  But we already did the ones in
	 * the rtable and cteList.
	 */
	if (parsetree->hasSubLinks)
		query_tree_walker(parsetree, acquireLocksOnSubLinks, &context,
						  QTW_IGNORE_RC_SUBQUERIES);
}

/*
 * Walker to find sublink subqueries for AcquireRewriteLocks
 */
static bool
acquireLocksOnSubLinks(Node *node, acquireLocksOnSubLinks_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
	{
		SubLink    *sub = (SubLink *) node;

		/* Do what we came for */
		AcquireRewriteLocks((Query *) sub->subselect,
							context->for_execute,
							false);
		/* Fall through to process lefthand args of SubLink */
	}

	/*
	 * Do NOT recurse into Query nodes, because AcquireRewriteLocks already
	 * processed subselects of subselects for us.
	 */
	return expression_tree_walker(node, acquireLocksOnSubLinks, context);
}


/*
 * rewriteRuleAction -
 *	  Rewrite the rule action with appropriate qualifiers (taken from
 *	  the triggering query).
 *
 * Input arguments:
 *	parsetree - original query
 *	rule_action - one action (query) of a rule
 *	rule_qual - WHERE condition of rule, or NULL if unconditional
 *	rt_index - RT index of result relation in original query
 *	event - type of rule event
 * Output arguments:
 *	*returning_flag - set true if we rewrite RETURNING clause in rule_action
 *					(must be initialized to false)
 * Return value:
 *	rewritten form of rule_action
 */
static Query *
rewriteRuleAction(Query *parsetree,
				  Query *rule_action,
				  Node *rule_qual,
				  int rt_index,
				  CmdType event,
				  bool *returning_flag)
{
	int			current_varno,
				new_varno;
	int			rt_length;
	Query	   *sub_action;
	Query	  **sub_action_ptr;
	acquireLocksOnSubLinks_context context;
	ListCell   *lc;

	context.for_execute = true;

	/*
	 * Make modifiable copies of rule action and qual (what we're passed are
	 * the stored versions in the relcache; don't touch 'em!).
	 */
	rule_action = copyObject(rule_action);
	rule_qual = copyObject(rule_qual);

	/*
	 * Acquire necessary locks and fix any deleted JOIN RTE entries.
	 */
	AcquireRewriteLocks(rule_action, true, false);
	(void) acquireLocksOnSubLinks(rule_qual, &context);

	current_varno = rt_index;
	rt_length = list_length(parsetree->rtable);
	new_varno = PRS2_NEW_VARNO + rt_length;

	/*
	 * Adjust rule action and qual to offset its varnos, so that we can merge
	 * its rtable with the main parsetree's rtable.
	 *
	 * If the rule action is an INSERT...SELECT, the OLD/NEW rtable entries
	 * will be in the SELECT part, and we have to modify that rather than the
	 * top-level INSERT (kluge!).
	 */
	sub_action = getInsertSelectQuery(rule_action, &sub_action_ptr);

	OffsetVarNodes((Node *) sub_action, rt_length, 0);
	OffsetVarNodes(rule_qual, rt_length, 0);
	/* but references to OLD should point at original rt_index */
	ChangeVarNodes((Node *) sub_action,
				   PRS2_OLD_VARNO + rt_length, rt_index, 0);
	ChangeVarNodes(rule_qual,
				   PRS2_OLD_VARNO + rt_length, rt_index, 0);

	/*
	 * Mark any subquery RTEs in the rule action as LATERAL if they contain
	 * Vars referring to the current query level (references to NEW/OLD).
	 * Those really are lateral references, but we've historically not
	 * required users to mark such subqueries with LATERAL explicitly.  But
	 * the planner will complain if such Vars exist in a non-LATERAL subquery,
	 * so we have to fix things up here.
	 */
	foreach(lc, sub_action->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (rte->rtekind == RTE_SUBQUERY && !rte->lateral &&
			contain_vars_of_level((Node *) rte->subquery, 1))
			rte->lateral = true;
	}

	/*
	 * Generate expanded rtable consisting of main parsetree's rtable plus
	 * rule action's rtable; this becomes the complete rtable for the rule
	 * action.  Some of the entries may be unused after we finish rewriting,
	 * but we leave them all in place to avoid having to adjust the query's
	 * varnos.  RT entries that are not referenced in the completed jointree
	 * will be ignored by the planner, so they do not affect query semantics.
	 *
	 * Also merge RTEPermissionInfo lists to ensure that all permissions are
	 * checked correctly.
	 *
	 * If the rule is INSTEAD, then the original query won't be executed at
	 * all, and so its rteperminfos must be preserved so that the executor
	 * will do the correct permissions checks on the relations referenced in
	 * it. This allows us to check that the caller has, say, insert-permission
	 * on a view, when the view is not semantically referenced at all in the
	 * resulting query.
	 *
	 * When a rule is not INSTEAD, the permissions checks done using the
	 * copied entries will be redundant with those done during execution of
	 * the original query, but we don't bother to treat that case differently.
	 *
	 * NOTE: because planner will destructively alter rtable and rteperminfos,
	 * we must ensure that rule action's lists are separate and shares no
	 * substructure with the main query's lists.  Hence do a deep copy here
	 * for both.
	 */
	{
		List	   *rtable_tail = sub_action->rtable;
		List	   *perminfos_tail = sub_action->rteperminfos;

		/*
		 * RewriteQuery relies on the fact that RT entries from the original
		 * query appear at the start of the expanded rtable, so we put the
		 * action's original table at the end of the list.
		 */
		sub_action->rtable = copyObject(parsetree->rtable);
		sub_action->rteperminfos = copyObject(parsetree->rteperminfos);
		CombineRangeTables(&sub_action->rtable, &sub_action->rteperminfos,
						   rtable_tail, perminfos_tail);
	}

	/*
	 * There could have been some SubLinks in parsetree's rtable, in which
	 * case we'd better mark the sub_action correctly.
	 */
	if (parsetree->hasSubLinks && !sub_action->hasSubLinks)
	{
		foreach(lc, parsetree->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

			switch (rte->rtekind)
			{
				case RTE_RELATION:
					sub_action->hasSubLinks =
						checkExprHasSubLink((Node *) rte->tablesample);
					break;
				case RTE_FUNCTION:
					sub_action->hasSubLinks =
						checkExprHasSubLink((Node *) rte->functions);
					break;
				case RTE_TABLEFUNC:
					sub_action->hasSubLinks =
						checkExprHasSubLink((Node *) rte->tablefunc);
					break;
				case RTE_VALUES:
					sub_action->hasSubLinks =
						checkExprHasSubLink((Node *) rte->values_lists);
					break;
				default:
					/* other RTE types don't contain bare expressions */
					break;
			}
			sub_action->hasSubLinks |=
				checkExprHasSubLink((Node *) rte->securityQuals);
			if (sub_action->hasSubLinks)
				break;			/* no need to keep scanning rtable */
		}
	}

	/*
	 * Also, we might have absorbed some RTEs with RLS conditions into the
	 * sub_action.  If so, mark it as hasRowSecurity, whether or not those
	 * RTEs will be referenced after we finish rewriting.  (Note: currently
	 * this is a no-op because RLS conditions aren't added till later, but it
	 * seems like good future-proofing to do this anyway.)
	 */
	sub_action->hasRowSecurity |= parsetree->hasRowSecurity;

	/*
	 * Each rule action's jointree should be the main parsetree's jointree
	 * plus that rule's jointree, but usually *without* the original rtindex
	 * that we're replacing (if present, which it won't be for INSERT). Note
	 * that if the rule action refers to OLD, its jointree will add a
	 * reference to rt_index.  If the rule action doesn't refer to OLD, but
	 * either the rule_qual or the user query quals do, then we need to keep
	 * the original rtindex in the jointree to provide data for the quals.  We
	 * don't want the original rtindex to be joined twice, however, so avoid
	 * keeping it if the rule action mentions it.
	 *
	 * As above, the action's jointree must not share substructure with the
	 * main parsetree's.
	 */
	if (sub_action->commandType != CMD_UTILITY)
	{
		bool		keeporig;
		List	   *newjointree;

		Assert(sub_action->jointree != NULL);
		keeporig = (!rangeTableEntry_used((Node *) sub_action->jointree,
										  rt_index, 0)) &&
			(rangeTableEntry_used(rule_qual, rt_index, 0) ||
			 rangeTableEntry_used(parsetree->jointree->quals, rt_index, 0));
		newjointree = adjustJoinTreeList(parsetree, !keeporig, rt_index);
		if (newjointree != NIL)
		{
			/*
			 * If sub_action is a setop, manipulating its jointree will do no
			 * good at all, because the jointree is dummy.  (Perhaps someday
			 * we could push the joining and quals down to the member
			 * statements of the setop?)
			 */
			if (sub_action->setOperations != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("conditional UNION/INTERSECT/EXCEPT statements are not implemented")));

			sub_action->jointree->fromlist =
				list_concat(newjointree, sub_action->jointree->fromlist);

			/*
			 * There could have been some SubLinks in newjointree, in which
			 * case we'd better mark the sub_action correctly.
			 */
			if (parsetree->hasSubLinks && !sub_action->hasSubLinks)
				sub_action->hasSubLinks =
					checkExprHasSubLink((Node *) newjointree);
		}
	}

	/*
	 * If the original query has any CTEs, copy them into the rule action. But
	 * we don't need them for a utility action.
	 */
	if (parsetree->cteList != NIL && sub_action->commandType != CMD_UTILITY)
	{
		/*
		 * Annoying implementation restriction: because CTEs are identified by
		 * name within a cteList, we can't merge a CTE from the original query
		 * if it has the same name as any CTE in the rule action.
		 *
		 * This could possibly be fixed by using some sort of internally
		 * generated ID, instead of names, to link CTE RTEs to their CTEs.
		 * However, decompiling the results would be quite confusing; note the
		 * merge of hasRecursive flags below, which could change the apparent
		 * semantics of such redundantly-named CTEs.
		 */
		foreach(lc, parsetree->cteList)
		{
			CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);
			ListCell   *lc2;

			foreach(lc2, sub_action->cteList)
			{
				CommonTableExpr *cte2 = (CommonTableExpr *) lfirst(lc2);

				if (strcmp(cte->ctename, cte2->ctename) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("WITH query name \"%s\" appears in both a rule action and the query being rewritten",
									cte->ctename)));
			}
		}

		/* OK, it's safe to combine the CTE lists */
		sub_action->cteList = list_concat(sub_action->cteList,
										  copyObject(parsetree->cteList));
		/* ... and don't forget about the associated flags */
		sub_action->hasRecursive |= parsetree->hasRecursive;
		sub_action->hasModifyingCTE |= parsetree->hasModifyingCTE;

		/*
		 * If rule_action is different from sub_action (i.e., the rule action
		 * is an INSERT...SELECT), then we might have just added some
		 * data-modifying CTEs that are not at the top query level.  This is
		 * disallowed by the parser and we mustn't generate such trees here
		 * either, so throw an error.
		 *
		 * Conceivably such cases could be supported by attaching the original
		 * query's CTEs to rule_action not sub_action.  But to do that, we'd
		 * have to increment ctelevelsup in RTEs and SubLinks copied from the
		 * original query.  For now, it doesn't seem worth the trouble.
		 */
		if (sub_action->hasModifyingCTE && rule_action != sub_action)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INSERT ... SELECT rule actions are not supported for queries having data-modifying statements in WITH")));
	}

	/*
	 * Event Qualification forces copying of parsetree and splitting into two
	 * queries one w/rule_qual, one w/NOT rule_qual. Also add user query qual
	 * onto rule action
	 */
	AddQual(sub_action, rule_qual);

	AddQual(sub_action, parsetree->jointree->quals);

	/*
	 * Rewrite new.attribute with right hand side of target-list entry for
	 * appropriate field name in insert/update.
	 *
	 * KLUGE ALERT: since ReplaceVarsFromTargetList returns a mutated copy, we
	 * can't just apply it to sub_action; we have to remember to update the
	 * sublink inside rule_action, too.
	 */
	if ((event == CMD_INSERT || event == CMD_UPDATE) &&
		sub_action->commandType != CMD_UTILITY)
	{
		sub_action = (Query *)
			ReplaceVarsFromTargetList((Node *) sub_action,
									  new_varno,
									  0,
									  rt_fetch(new_varno, sub_action->rtable),
									  parsetree->targetList,
									  sub_action->resultRelation,
									  (event == CMD_UPDATE) ?
									  REPLACEVARS_CHANGE_VARNO :
									  REPLACEVARS_SUBSTITUTE_NULL,
									  current_varno,
									  NULL);
		if (sub_action_ptr)
			*sub_action_ptr = sub_action;
		else
			rule_action = sub_action;
	}

	/*
	 * If rule_action has a RETURNING clause, then either throw it away if the
	 * triggering query has no RETURNING clause, or rewrite it to emit what
	 * the triggering query's RETURNING clause asks for.  Throw an error if
	 * more than one rule has a RETURNING clause.
	 */
	if (!parsetree->returningList)
		rule_action->returningList = NIL;
	else if (rule_action->returningList)
	{
		if (*returning_flag)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot have RETURNING lists in multiple rules")));
		*returning_flag = true;
		rule_action->returningList = (List *)
			ReplaceVarsFromTargetList((Node *) parsetree->returningList,
									  parsetree->resultRelation,
									  0,
									  rt_fetch(parsetree->resultRelation,
											   parsetree->rtable),
									  rule_action->returningList,
									  rule_action->resultRelation,
									  REPLACEVARS_REPORT_ERROR,
									  0,
									  &rule_action->hasSubLinks);

		/* use triggering query's aliases for OLD and NEW in RETURNING list */
		rule_action->returningOldAlias = parsetree->returningOldAlias;
		rule_action->returningNewAlias = parsetree->returningNewAlias;

		/*
		 * There could have been some SubLinks in parsetree's returningList,
		 * in which case we'd better mark the rule_action correctly.
		 */
		if (parsetree->hasSubLinks && !rule_action->hasSubLinks)
			rule_action->hasSubLinks =
				checkExprHasSubLink((Node *) rule_action->returningList);
	}

	return rule_action;
}

/*
 * Copy the query's jointree list, and optionally attempt to remove any
 * occurrence of the given rt_index as a top-level join item (we do not look
 * for it within join items; this is OK because we are only expecting to find
 * it as an UPDATE or DELETE target relation, which will be at the top level
 * of the join).  Returns modified jointree list --- this is a separate copy
 * sharing no nodes with the original.
 */
static List *
adjustJoinTreeList(Query *parsetree, bool removert, int rt_index)
{
	List	   *newjointree = copyObject(parsetree->jointree->fromlist);
	ListCell   *l;

	if (removert)
	{
		foreach(l, newjointree)
		{
			RangeTblRef *rtr = lfirst(l);

			if (IsA(rtr, RangeTblRef) &&
				rtr->rtindex == rt_index)
			{
				newjointree = foreach_delete_current(newjointree, l);
				break;
			}
		}
	}
	return newjointree;
}


/*
 * rewriteTargetListIU - rewrite INSERT/UPDATE targetlist into standard form
 *
 * This has the following responsibilities:
 *
 * 1. For an INSERT, add tlist entries to compute default values for any
 * attributes that have defaults and are not assigned to in the given tlist.
 * (We do not insert anything for default-less attributes, however.  The
 * planner will later insert NULLs for them, but there's no reason to slow
 * down rewriter processing with extra tlist nodes.)  Also, for both INSERT
 * and UPDATE, replace explicit DEFAULT specifications with column default
 * expressions.
 *
 * 2. Merge multiple entries for the same target attribute, or declare error
 * if we can't.  Multiple entries are only allowed for INSERT/UPDATE of
 * portions of an array or record field, for example
 *			UPDATE table SET foo[2] = 42, foo[4] = 43;
 * We can merge such operations into a single assignment op.  Essentially,
 * the expression we want to produce in this case is like
 *		foo = array_set_element(array_set_element(foo, 2, 42), 4, 43)
 *
 * 3. Sort the tlist into standard order: non-junk fields in order by resno,
 * then junk fields (these in no particular order).
 *
 * We must do items 1 and 2 before firing rewrite rules, else rewritten
 * references to NEW.foo will produce wrong or incomplete results.  Item 3
 * is not needed for rewriting, but it is helpful for the planner, and we
 * can do it essentially for free while handling the other items.
 *
 * If values_rte is non-NULL (i.e., we are doing a multi-row INSERT using
 * values from a VALUES RTE), we populate *unused_values_attrnos with the
 * attribute numbers of any unused columns from the VALUES RTE.  This can
 * happen for identity and generated columns whose targetlist entries are
 * replaced with generated expressions (if INSERT ... OVERRIDING USER VALUE is
 * used, or all the values to be inserted are DEFAULT).  This information is
 * required by rewriteValuesRTE() to handle any DEFAULT items in the unused
 * columns.  The caller must have initialized *unused_values_attrnos to NULL.
 */
static List *
rewriteTargetListIU(List *targetList,
					CmdType commandType,
					OverridingKind override,
					Relation target_relation,
					RangeTblEntry *values_rte,
					int values_rte_index,
					Bitmapset **unused_values_attrnos)
{
	TargetEntry **new_tles;
	List	   *new_tlist = NIL;
	List	   *junk_tlist = NIL;
	Form_pg_attribute att_tup;
	int			attrno,
				next_junk_attrno,
				numattrs;
	ListCell   *temp;
	Bitmapset  *default_only_cols = NULL;

	/*
	 * We process the normal (non-junk) attributes by scanning the input tlist
	 * once and transferring TLEs into an array, then scanning the array to
	 * build an output tlist.  This avoids O(N^2) behavior for large numbers
	 * of attributes.
	 *
	 * Junk attributes are tossed into a separate list during the same tlist
	 * scan, then appended to the reconstructed tlist.
	 */
	numattrs = RelationGetNumberOfAttributes(target_relation);
	new_tles = (TargetEntry **) palloc0(numattrs * sizeof(TargetEntry *));
	next_junk_attrno = numattrs + 1;

	foreach(temp, targetList)
	{
		TargetEntry *old_tle = (TargetEntry *) lfirst(temp);

		if (!old_tle->resjunk)
		{
			/* Normal attr: stash it into new_tles[] */
			attrno = old_tle->resno;
			if (attrno < 1 || attrno > numattrs)
				elog(ERROR, "bogus resno %d in targetlist", attrno);
			att_tup = TupleDescAttr(target_relation->rd_att, attrno - 1);

			/* We can (and must) ignore deleted attributes */
			if (att_tup->attisdropped)
				continue;

			/* Merge with any prior assignment to same attribute */
			new_tles[attrno - 1] =
				process_matched_tle(old_tle,
									new_tles[attrno - 1],
									NameStr(att_tup->attname));
		}
		else
		{
			/*
			 * Copy all resjunk tlist entries to junk_tlist, and assign them
			 * resnos above the last real resno.
			 *
			 * Typical junk entries include ORDER BY or GROUP BY expressions
			 * (are these actually possible in an INSERT or UPDATE?), system
			 * attribute references, etc.
			 */

			/* Get the resno right, but don't copy unnecessarily */
			if (old_tle->resno != next_junk_attrno)
			{
				old_tle = flatCopyTargetEntry(old_tle);
				old_tle->resno = next_junk_attrno;
			}
			junk_tlist = lappend(junk_tlist, old_tle);
			next_junk_attrno++;
		}
	}

	for (attrno = 1; attrno <= numattrs; attrno++)
	{
		TargetEntry *new_tle = new_tles[attrno - 1];
		bool		apply_default;

		att_tup = TupleDescAttr(target_relation->rd_att, attrno - 1);

		/* We can (and must) ignore deleted attributes */
		if (att_tup->attisdropped)
			continue;

		/*
		 * Handle the two cases where we need to insert a default expression:
		 * it's an INSERT and there's no tlist entry for the column, or the
		 * tlist entry is a DEFAULT placeholder node.
		 */
		apply_default = ((new_tle == NULL && commandType == CMD_INSERT) ||
						 (new_tle && new_tle->expr && IsA(new_tle->expr, SetToDefault)));

		if (commandType == CMD_INSERT)
		{
			int			values_attrno = 0;

			/* Source attribute number for values that come from a VALUES RTE */
			if (values_rte && new_tle && IsA(new_tle->expr, Var))
			{
				Var		   *var = (Var *) new_tle->expr;

				if (var->varno == values_rte_index)
					values_attrno = var->varattno;
			}

			/*
			 * Can only insert DEFAULT into GENERATED ALWAYS identity columns,
			 * unless either OVERRIDING USER VALUE or OVERRIDING SYSTEM VALUE
			 * is specified.
			 */
			if (att_tup->attidentity == ATTRIBUTE_IDENTITY_ALWAYS && !apply_default)
			{
				if (override == OVERRIDING_USER_VALUE)
					apply_default = true;
				else if (override != OVERRIDING_SYSTEM_VALUE)
				{
					/*
					 * If this column's values come from a VALUES RTE, test
					 * whether it contains only SetToDefault items.  Since the
					 * VALUES list might be quite large, we arrange to only
					 * scan it once.
					 */
					if (values_attrno != 0)
					{
						if (default_only_cols == NULL)
							default_only_cols = findDefaultOnlyColumns(values_rte);

						if (bms_is_member(values_attrno, default_only_cols))
							apply_default = true;
					}

					if (!apply_default)
						ereport(ERROR,
								(errcode(ERRCODE_GENERATED_ALWAYS),
								 errmsg("cannot insert a non-DEFAULT value into column \"%s\"",
										NameStr(att_tup->attname)),
								 errdetail("Column \"%s\" is an identity column defined as GENERATED ALWAYS.",
										   NameStr(att_tup->attname)),
								 errhint("Use OVERRIDING SYSTEM VALUE to override.")));
				}
			}

			/*
			 * Although inserting into a GENERATED BY DEFAULT identity column
			 * is allowed, apply the default if OVERRIDING USER VALUE is
			 * specified.
			 */
			if (att_tup->attidentity == ATTRIBUTE_IDENTITY_BY_DEFAULT &&
				override == OVERRIDING_USER_VALUE)
				apply_default = true;

			/*
			 * Can only insert DEFAULT into generated columns.  (The
			 * OVERRIDING clause does not apply to generated columns, so we
			 * don't consider it here.)
			 */
			if (att_tup->attgenerated && !apply_default)
			{
				/*
				 * If this column's values come from a VALUES RTE, test
				 * whether it contains only SetToDefault items, as above.
				 */
				if (values_attrno != 0)
				{
					if (default_only_cols == NULL)
						default_only_cols = findDefaultOnlyColumns(values_rte);

					if (bms_is_member(values_attrno, default_only_cols))
						apply_default = true;
				}

				if (!apply_default)
					ereport(ERROR,
							(errcode(ERRCODE_GENERATED_ALWAYS),
							 errmsg("cannot insert a non-DEFAULT value into column \"%s\"",
									NameStr(att_tup->attname)),
							 errdetail("Column \"%s\" is a generated column.",
									   NameStr(att_tup->attname))));
			}

			/*
			 * For an INSERT from a VALUES RTE, return the attribute numbers
			 * of any VALUES columns that will no longer be used (due to the
			 * targetlist entry being replaced by a default expression).
			 */
			if (values_attrno != 0 && apply_default && unused_values_attrnos)
				*unused_values_attrnos = bms_add_member(*unused_values_attrnos,
														values_attrno);
		}

		/*
		 * Updates to identity and generated columns follow the same rules as
		 * above, except that UPDATE doesn't admit OVERRIDING clauses.  Also,
		 * the source can't be a VALUES RTE, so we needn't consider that.
		 */
		if (commandType == CMD_UPDATE)
		{
			if (att_tup->attidentity == ATTRIBUTE_IDENTITY_ALWAYS &&
				new_tle && !apply_default)
				ereport(ERROR,
						(errcode(ERRCODE_GENERATED_ALWAYS),
						 errmsg("column \"%s\" can only be updated to DEFAULT",
								NameStr(att_tup->attname)),
						 errdetail("Column \"%s\" is an identity column defined as GENERATED ALWAYS.",
								   NameStr(att_tup->attname))));

			if (att_tup->attgenerated && new_tle && !apply_default)
				ereport(ERROR,
						(errcode(ERRCODE_GENERATED_ALWAYS),
						 errmsg("column \"%s\" can only be updated to DEFAULT",
								NameStr(att_tup->attname)),
						 errdetail("Column \"%s\" is a generated column.",
								   NameStr(att_tup->attname))));
		}

		if (att_tup->attgenerated)
		{
			/*
			 * virtual generated column stores a null value; stored generated
			 * column will be fixed in executor
			 */
			new_tle = NULL;
		}
		else if (apply_default)
		{
			Node	   *new_expr;

			new_expr = build_column_default(target_relation, attrno);

			/*
			 * If there is no default (ie, default is effectively NULL), we
			 * can omit the tlist entry in the INSERT case, since the planner
			 * can insert a NULL for itself, and there's no point in spending
			 * any more rewriter cycles on the entry.  But in the UPDATE case
			 * we've got to explicitly set the column to NULL.
			 */
			if (!new_expr)
			{
				if (commandType == CMD_INSERT)
					new_tle = NULL;
				else
					new_expr = coerce_null_to_domain(att_tup->atttypid,
													 att_tup->atttypmod,
													 att_tup->attcollation,
													 att_tup->attlen,
													 att_tup->attbyval);
			}

			if (new_expr)
				new_tle = makeTargetEntry((Expr *) new_expr,
										  attrno,
										  pstrdup(NameStr(att_tup->attname)),
										  false);
		}

		if (new_tle)
			new_tlist = lappend(new_tlist, new_tle);
	}

	pfree(new_tles);

	return list_concat(new_tlist, junk_tlist);
}


/*
 * Convert a matched TLE from the original tlist into a correct new TLE.
 *
 * This routine detects and handles multiple assignments to the same target
 * attribute.  (The attribute name is needed only for error messages.)
 */
static TargetEntry *
process_matched_tle(TargetEntry *src_tle,
					TargetEntry *prior_tle,
					const char *attrName)
{
	TargetEntry *result;
	CoerceToDomain *coerce_expr = NULL;
	Node	   *src_expr;
	Node	   *prior_expr;
	Node	   *src_input;
	Node	   *prior_input;
	Node	   *priorbottom;
	Node	   *newexpr;

	if (prior_tle == NULL)
	{
		/*
		 * Normal case where this is the first assignment to the attribute.
		 */
		return src_tle;
	}

	/*----------
	 * Multiple assignments to same attribute.  Allow only if all are
	 * FieldStore or SubscriptingRef assignment operations.  This is a bit
	 * tricky because what we may actually be looking at is a nest of
	 * such nodes; consider
	 *		UPDATE tab SET col.fld1.subfld1 = x, col.fld2.subfld2 = y
	 * The two expressions produced by the parser will look like
	 *		FieldStore(col, fld1, FieldStore(placeholder, subfld1, x))
	 *		FieldStore(col, fld2, FieldStore(placeholder, subfld2, y))
	 * However, we can ignore the substructure and just consider the top
	 * FieldStore or SubscriptingRef from each assignment, because it works to
	 * combine these as
	 *		FieldStore(FieldStore(col, fld1,
	 *							  FieldStore(placeholder, subfld1, x)),
	 *				   fld2, FieldStore(placeholder, subfld2, y))
	 * Note the leftmost expression goes on the inside so that the
	 * assignments appear to occur left-to-right.
	 *
	 * For FieldStore, instead of nesting we can generate a single
	 * FieldStore with multiple target fields.  We must nest when
	 * SubscriptingRefs are involved though.
	 *
	 * As a further complication, the destination column might be a domain,
	 * resulting in each assignment containing a CoerceToDomain node over a
	 * FieldStore or SubscriptingRef.  These should have matching target
	 * domains, so we strip them and reconstitute a single CoerceToDomain over
	 * the combined FieldStore/SubscriptingRef nodes.  (Notice that this has
	 * the result that the domain's checks are applied only after we do all
	 * the field or element updates, not after each one.  This is desirable.)
	 *----------
	 */
	src_expr = (Node *) src_tle->expr;
	prior_expr = (Node *) prior_tle->expr;

	if (src_expr && IsA(src_expr, CoerceToDomain) &&
		prior_expr && IsA(prior_expr, CoerceToDomain) &&
		((CoerceToDomain *) src_expr)->resulttype ==
		((CoerceToDomain *) prior_expr)->resulttype)
	{
		/* we assume without checking that resulttypmod/resultcollid match */
		coerce_expr = (CoerceToDomain *) src_expr;
		src_expr = (Node *) ((CoerceToDomain *) src_expr)->arg;
		prior_expr = (Node *) ((CoerceToDomain *) prior_expr)->arg;
	}

	src_input = get_assignment_input(src_expr);
	prior_input = get_assignment_input(prior_expr);
	if (src_input == NULL ||
		prior_input == NULL ||
		exprType(src_expr) != exprType(prior_expr))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("multiple assignments to same column \"%s\"",
						attrName)));

	/*
	 * Prior TLE could be a nest of assignments if we do this more than once.
	 */
	priorbottom = prior_input;
	for (;;)
	{
		Node	   *newbottom = get_assignment_input(priorbottom);

		if (newbottom == NULL)
			break;				/* found the original Var reference */
		priorbottom = newbottom;
	}
	if (!equal(priorbottom, src_input))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("multiple assignments to same column \"%s\"",
						attrName)));

	/*
	 * Looks OK to nest 'em.
	 */
	if (IsA(src_expr, FieldStore))
	{
		FieldStore *fstore = makeNode(FieldStore);

		if (IsA(prior_expr, FieldStore))
		{
			/* combine the two */
			memcpy(fstore, prior_expr, sizeof(FieldStore));
			fstore->newvals =
				list_concat_copy(((FieldStore *) prior_expr)->newvals,
								 ((FieldStore *) src_expr)->newvals);
			fstore->fieldnums =
				list_concat_copy(((FieldStore *) prior_expr)->fieldnums,
								 ((FieldStore *) src_expr)->fieldnums);
		}
		else
		{
			/* general case, just nest 'em */
			memcpy(fstore, src_expr, sizeof(FieldStore));
			fstore->arg = (Expr *) prior_expr;
		}
		newexpr = (Node *) fstore;
	}
	else if (IsA(src_expr, SubscriptingRef))
	{
		SubscriptingRef *sbsref = makeNode(SubscriptingRef);

		memcpy(sbsref, src_expr, sizeof(SubscriptingRef));
		sbsref->refexpr = (Expr *) prior_expr;
		newexpr = (Node *) sbsref;
	}
	else
	{
		elog(ERROR, "cannot happen");
		newexpr = NULL;
	}

	if (coerce_expr)
	{
		/* put back the CoerceToDomain */
		CoerceToDomain *newcoerce = makeNode(CoerceToDomain);

		memcpy(newcoerce, coerce_expr, sizeof(CoerceToDomain));
		newcoerce->arg = (Expr *) newexpr;
		newexpr = (Node *) newcoerce;
	}

	result = flatCopyTargetEntry(src_tle);
	result->expr = (Expr *) newexpr;
	return result;
}

/*
 * If node is an assignment node, return its input; else return NULL
 */
static Node *
get_assignment_input(Node *node)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, FieldStore))
	{
		FieldStore *fstore = (FieldStore *) node;

		return (Node *) fstore->arg;
	}
	else if (IsA(node, SubscriptingRef))
	{
		SubscriptingRef *sbsref = (SubscriptingRef *) node;

		if (sbsref->refassgnexpr == NULL)
			return NULL;

		return (Node *) sbsref->refexpr;
	}

	return NULL;
}

/*
 * Make an expression tree for the default value for a column.
 *
 * If there is no default, return a NULL instead.
 */
Node *
build_column_default(Relation rel, int attrno)
{
	TupleDesc	rd_att = rel->rd_att;
	Form_pg_attribute att_tup = TupleDescAttr(rd_att, attrno - 1);
	Oid			atttype = att_tup->atttypid;
	int32		atttypmod = att_tup->atttypmod;
	Node	   *expr = NULL;
	Oid			exprtype;

	if (att_tup->attidentity)
	{
		NextValueExpr *nve = makeNode(NextValueExpr);

		nve->seqid = getIdentitySequence(rel, attrno, false);
		nve->typeId = att_tup->atttypid;

		return (Node *) nve;
	}

	/*
	 * If relation has a default for this column, fetch that expression.
	 */
	if (att_tup->atthasdef)
	{
		expr = TupleDescGetDefault(rd_att, attrno);
		if (expr == NULL)
			elog(ERROR, "default expression not found for attribute %d of relation \"%s\"",
				 attrno, RelationGetRelationName(rel));
	}

	/*
	 * No per-column default, so look for a default for the type itself.  But
	 * not for generated columns.
	 */
	if (expr == NULL && !att_tup->attgenerated)
		expr = get_typdefault(atttype);

	if (expr == NULL)
		return NULL;			/* No default anywhere */

	/*
	 * Make sure the value is coerced to the target column type; this will
	 * generally be true already, but there seem to be some corner cases
	 * involving domain defaults where it might not be true. This should match
	 * the parser's processing of non-defaulted expressions --- see
	 * transformAssignedExpr().
	 */
	exprtype = exprType(expr);

	expr = coerce_to_target_type(NULL,	/* no UNKNOWN params here */
								 expr, exprtype,
								 atttype, atttypmod,
								 COERCION_ASSIGNMENT,
								 COERCE_IMPLICIT_CAST,
								 -1);
	if (expr == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("column \"%s\" is of type %s"
						" but default expression is of type %s",
						NameStr(att_tup->attname),
						format_type_be(atttype),
						format_type_be(exprtype)),
				 errhint("You will need to rewrite or cast the expression.")));

	return expr;
}


/* Does VALUES RTE contain any SetToDefault items? */
static bool
searchForDefault(RangeTblEntry *rte)
{
	ListCell   *lc;

	foreach(lc, rte->values_lists)
	{
		List	   *sublist = (List *) lfirst(lc);
		ListCell   *lc2;

		foreach(lc2, sublist)
		{
			Node	   *col = (Node *) lfirst(lc2);

			if (IsA(col, SetToDefault))
				return true;
		}
	}
	return false;
}


/*
 * Search a VALUES RTE for columns that contain only SetToDefault items,
 * returning a Bitmapset containing the attribute numbers of any such columns.
 */
static Bitmapset *
findDefaultOnlyColumns(RangeTblEntry *rte)
{
	Bitmapset  *default_only_cols = NULL;
	ListCell   *lc;

	foreach(lc, rte->values_lists)
	{
		List	   *sublist = (List *) lfirst(lc);
		ListCell   *lc2;
		int			i;

		if (default_only_cols == NULL)
		{
			/* Populate the initial result bitmap from the first row */
			i = 0;
			foreach(lc2, sublist)
			{
				Node	   *col = (Node *) lfirst(lc2);

				i++;
				if (IsA(col, SetToDefault))
					default_only_cols = bms_add_member(default_only_cols, i);
			}
		}
		else
		{
			/* Update the result bitmap from this next row */
			i = 0;
			foreach(lc2, sublist)
			{
				Node	   *col = (Node *) lfirst(lc2);

				i++;
				if (!IsA(col, SetToDefault))
					default_only_cols = bms_del_member(default_only_cols, i);
			}
		}

		/*
		 * If no column in the rows read so far contains only DEFAULT items,
		 * we are done.
		 */
		if (bms_is_empty(default_only_cols))
			break;
	}

	return default_only_cols;
}


/*
 * When processing INSERT ... VALUES with a VALUES RTE (ie, multiple VALUES
 * lists), we have to replace any DEFAULT items in the VALUES lists with
 * the appropriate default expressions.  The other aspects of targetlist
 * rewriting need be applied only to the query's targetlist proper.
 *
 * For an auto-updatable view, each DEFAULT item in the VALUES list is
 * replaced with the default from the view, if it has one.  Otherwise it is
 * left untouched so that the underlying base relation's default can be
 * applied instead (when we later recurse to here after rewriting the query
 * to refer to the base relation instead of the view).
 *
 * For other types of relation, including rule- and trigger-updatable views,
 * all DEFAULT items are replaced, and if the target relation doesn't have a
 * default, the value is explicitly set to NULL.
 *
 * Also, if a DEFAULT item is found in a column mentioned in unused_cols,
 * it is explicitly set to NULL.  This happens for columns in the VALUES RTE
 * whose corresponding targetlist entries have already been replaced with the
 * relation's default expressions, so that any values in those columns of the
 * VALUES RTE are no longer used.  This can happen for identity and generated
 * columns (if INSERT ... OVERRIDING USER VALUE is used, or all the values to
 * be inserted are DEFAULT).  In principle we could replace all entries in
 * such a column with NULL, whether DEFAULT or not; but it doesn't seem worth
 * the trouble.
 *
 * Note that we may have subscripted or field assignment targetlist entries,
 * as well as more complex expressions from already-replaced DEFAULT items if
 * we have recursed to here for an auto-updatable view. However, it ought to
 * be impossible for such entries to have DEFAULTs assigned to them, except
 * for unused columns, as described above --- we should only have to replace
 * DEFAULT items for targetlist entries that contain simple Vars referencing
 * the VALUES RTE, or which are no longer referred to by the targetlist.
 *
 * Returns true if all DEFAULT items were replaced, and false if some were
 * left untouched.
 */
static bool
rewriteValuesRTE(Query *parsetree, RangeTblEntry *rte, int rti,
				 Relation target_relation,
				 Bitmapset *unused_cols)
{
	List	   *newValues;
	ListCell   *lc;
	bool		isAutoUpdatableView;
	bool		allReplaced;
	int			numattrs;
	int		   *attrnos;

	/* Steps below are not sensible for non-INSERT queries */
	Assert(parsetree->commandType == CMD_INSERT);
	Assert(rte->rtekind == RTE_VALUES);

	/*
	 * Rebuilding all the lists is a pretty expensive proposition in a big
	 * VALUES list, and it's a waste of time if there aren't any DEFAULT
	 * placeholders.  So first scan to see if there are any.
	 */
	if (!searchForDefault(rte))
		return true;			/* nothing to do */

	/*
	 * Scan the targetlist for entries referring to the VALUES RTE, and note
	 * the target attributes. As noted above, we should only need to do this
	 * for targetlist entries containing simple Vars --- nothing else in the
	 * VALUES RTE should contain DEFAULT items (except possibly for unused
	 * columns), and we complain if such a thing does occur.
	 */
	numattrs = list_length(linitial(rte->values_lists));
	attrnos = (int *) palloc0(numattrs * sizeof(int));

	foreach(lc, parsetree->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		if (IsA(tle->expr, Var))
		{
			Var		   *var = (Var *) tle->expr;

			if (var->varno == rti)
			{
				int			attrno = var->varattno;

				Assert(attrno >= 1 && attrno <= numattrs);
				attrnos[attrno - 1] = tle->resno;
			}
		}
	}

	/*
	 * Check if the target relation is an auto-updatable view, in which case
	 * unresolved defaults will be left untouched rather than being set to
	 * NULL.
	 */
	isAutoUpdatableView = false;
	if (target_relation->rd_rel->relkind == RELKIND_VIEW &&
		!view_has_instead_trigger(target_relation, CMD_INSERT, NIL))
	{
		List	   *locks;
		bool		hasUpdate;
		bool		found;
		ListCell   *l;

		/* Look for an unconditional DO INSTEAD rule */
		locks = matchLocks(CMD_INSERT, target_relation,
						   parsetree->resultRelation, parsetree, &hasUpdate);

		found = false;
		foreach(l, locks)
		{
			RewriteRule *rule_lock = (RewriteRule *) lfirst(l);

			if (rule_lock->isInstead &&
				rule_lock->qual == NULL)
			{
				found = true;
				break;
			}
		}

		/*
		 * If we didn't find an unconditional DO INSTEAD rule, assume that the
		 * view is auto-updatable.  If it isn't, rewriteTargetView() will
		 * throw an error.
		 */
		if (!found)
			isAutoUpdatableView = true;
	}

	newValues = NIL;
	allReplaced = true;
	foreach(lc, rte->values_lists)
	{
		List	   *sublist = (List *) lfirst(lc);
		List	   *newList = NIL;
		ListCell   *lc2;
		int			i;

		Assert(list_length(sublist) == numattrs);

		i = 0;
		foreach(lc2, sublist)
		{
			Node	   *col = (Node *) lfirst(lc2);
			int			attrno = attrnos[i++];

			if (IsA(col, SetToDefault))
			{
				Form_pg_attribute att_tup;
				Node	   *new_expr;

				/*
				 * If this column isn't used, just replace the DEFAULT with
				 * NULL (attrno will be 0 in this case because the targetlist
				 * entry will have been replaced by the default expression).
				 */
				if (bms_is_member(i, unused_cols))
				{
					SetToDefault *def = (SetToDefault *) col;

					newList = lappend(newList,
									  makeNullConst(def->typeId,
													def->typeMod,
													def->collation));
					continue;
				}

				if (attrno == 0)
					elog(ERROR, "cannot set value in column %d to DEFAULT", i);
				Assert(attrno > 0 && attrno <= target_relation->rd_att->natts);
				att_tup = TupleDescAttr(target_relation->rd_att, attrno - 1);

				if (!att_tup->attisdropped)
					new_expr = build_column_default(target_relation, attrno);
				else
					new_expr = NULL;	/* force a NULL if dropped */

				/*
				 * If there is no default (ie, default is effectively NULL),
				 * we've got to explicitly set the column to NULL, unless the
				 * target relation is an auto-updatable view.
				 */
				if (!new_expr)
				{
					if (isAutoUpdatableView)
					{
						/* Leave the value untouched */
						newList = lappend(newList, col);
						allReplaced = false;
						continue;
					}

					new_expr = coerce_null_to_domain(att_tup->atttypid,
													 att_tup->atttypmod,
													 att_tup->attcollation,
													 att_tup->attlen,
													 att_tup->attbyval);
				}
				newList = lappend(newList, new_expr);
			}
			else
				newList = lappend(newList, col);
		}
		newValues = lappend(newValues, newList);
	}
	rte->values_lists = newValues;

	pfree(attrnos);

	return allReplaced;
}

/*
 * Mop up any remaining DEFAULT items in the given VALUES RTE by
 * replacing them with NULL constants.
 *
 * This is used for the product queries generated by DO ALSO rules attached to
 * an auto-updatable view.  The action can't depend on the "target relation"
 * since the product query might not have one (it needn't be an INSERT).
 * Essentially, such queries are treated as being attached to a rule-updatable
 * view.
 */
static void
rewriteValuesRTEToNulls(Query *parsetree, RangeTblEntry *rte)
{
	List	   *newValues;
	ListCell   *lc;

	newValues = NIL;
	foreach(lc, rte->values_lists)
	{
		List	   *sublist = (List *) lfirst(lc);
		List	   *newList = NIL;
		ListCell   *lc2;

		foreach(lc2, sublist)
		{
			Node	   *col = (Node *) lfirst(lc2);

			if (IsA(col, SetToDefault))
			{
				SetToDefault *def = (SetToDefault *) col;

				newList = lappend(newList, makeNullConst(def->typeId,
														 def->typeMod,
														 def->collation));
			}
			else
				newList = lappend(newList, col);
		}
		newValues = lappend(newValues, newList);
	}
	rte->values_lists = newValues;
}


/*
 * matchLocks -
 *	  match a relation's list of locks and returns the matching rules
 */
static List *
matchLocks(CmdType event,
		   Relation relation,
		   int varno,
		   Query *parsetree,
		   bool *hasUpdate)
{
	RuleLock   *rulelocks = relation->rd_rules;
	List	   *matching_locks = NIL;
	int			nlocks;
	int			i;

	if (rulelocks == NULL)
		return NIL;

	if (parsetree->commandType != CMD_SELECT)
	{
		if (parsetree->resultRelation != varno)
			return NIL;
	}

	nlocks = rulelocks->numLocks;

	for (i = 0; i < nlocks; i++)
	{
		RewriteRule *oneLock = rulelocks->rules[i];

		if (oneLock->event == CMD_UPDATE)
			*hasUpdate = true;

		/*
		 * Suppress ON INSERT/UPDATE/DELETE rules that are disabled or
		 * configured to not fire during the current session's replication
		 * role. ON SELECT rules will always be applied in order to keep views
		 * working even in LOCAL or REPLICA role.
		 */
		if (oneLock->event != CMD_SELECT)
		{
			if (SessionReplicationRole == SESSION_REPLICATION_ROLE_REPLICA)
			{
				if (oneLock->enabled == RULE_FIRES_ON_ORIGIN ||
					oneLock->enabled == RULE_DISABLED)
					continue;
			}
			else				/* ORIGIN or LOCAL ROLE */
			{
				if (oneLock->enabled == RULE_FIRES_ON_REPLICA ||
					oneLock->enabled == RULE_DISABLED)
					continue;
			}

			/* Non-SELECT rules are not supported for MERGE */
			if (parsetree->commandType == CMD_MERGE)
				ereport(ERROR,
						errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot execute MERGE on relation \"%s\"",
							   RelationGetRelationName(relation)),
						errdetail("MERGE is not supported for relations with rules."));
		}

		if (oneLock->event == event)
		{
			if (parsetree->commandType != CMD_SELECT ||
				rangeTableEntry_used((Node *) parsetree, varno, 0))
				matching_locks = lappend(matching_locks, oneLock);
		}
	}

	return matching_locks;
}


/*
 * ApplyRetrieveRule - expand an ON SELECT rule
 */
static Query *
ApplyRetrieveRule(Query *parsetree,
				  RewriteRule *rule,
				  int rt_index,
				  Relation relation,
				  List *activeRIRs)
{
	Query	   *rule_action;
	RangeTblEntry *rte;
	RowMarkClause *rc;
	int			numCols;

	if (list_length(rule->actions) != 1)
		elog(ERROR, "expected just one rule action");
	if (rule->qual != NULL)
		elog(ERROR, "cannot handle qualified ON SELECT rule");

	/* Check if the expansion of non-system views are restricted */
	if (unlikely((restrict_nonsystem_relation_kind & RESTRICT_RELKIND_VIEW) != 0 &&
				 RelationGetRelid(relation) >= FirstNormalObjectId))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("access to non-system view \"%s\" is restricted",
						RelationGetRelationName(relation))));

	if (rt_index == parsetree->resultRelation)
	{
		/*
		 * We have a view as the result relation of the query, and it wasn't
		 * rewritten by any rule.  This case is supported if there is an
		 * INSTEAD OF trigger that will trap attempts to insert/update/delete
		 * view rows.  The executor will check that; for the moment just plow
		 * ahead.  We have two cases:
		 *
		 * For INSERT, we needn't do anything.  The unmodified RTE will serve
		 * fine as the result relation.
		 *
		 * For UPDATE/DELETE/MERGE, we need to expand the view so as to have
		 * source data for the operation.  But we also need an unmodified RTE
		 * to serve as the target.  So, copy the RTE and add the copy to the
		 * rangetable.  Note that the copy does not get added to the jointree.
		 * Also note that there's a hack in fireRIRrules to avoid calling this
		 * function again when it arrives at the copied RTE.
		 */
		if (parsetree->commandType == CMD_INSERT)
			return parsetree;
		else if (parsetree->commandType == CMD_UPDATE ||
				 parsetree->commandType == CMD_DELETE ||
				 parsetree->commandType == CMD_MERGE)
		{
			RangeTblEntry *newrte;
			Var		   *var;
			TargetEntry *tle;

			rte = rt_fetch(rt_index, parsetree->rtable);
			newrte = copyObject(rte);
			parsetree->rtable = lappend(parsetree->rtable, newrte);
			parsetree->resultRelation = list_length(parsetree->rtable);
			/* parsetree->mergeTargetRelation unchanged (use expanded view) */

			/*
			 * For the most part, Vars referencing the view should remain as
			 * they are, meaning that they implicitly represent OLD values.
			 * But in the RETURNING list if any, we want such Vars to
			 * represent NEW values, so change them to reference the new RTE.
			 *
			 * Since ChangeVarNodes scribbles on the tree in-place, copy the
			 * RETURNING list first for safety.
			 */
			parsetree->returningList = copyObject(parsetree->returningList);
			ChangeVarNodes((Node *) parsetree->returningList, rt_index,
						   parsetree->resultRelation, 0);

			/*
			 * To allow the executor to compute the original view row to pass
			 * to the INSTEAD OF trigger, we add a resjunk whole-row Var
			 * referencing the original RTE.  This will later get expanded
			 * into a RowExpr computing all the OLD values of the view row.
			 */
			var = makeWholeRowVar(rte, rt_index, 0, false);
			tle = makeTargetEntry((Expr *) var,
								  list_length(parsetree->targetList) + 1,
								  pstrdup("wholerow"),
								  true);

			parsetree->targetList = lappend(parsetree->targetList, tle);

			/* Now, continue with expanding the original view RTE */
		}
		else
			elog(ERROR, "unrecognized commandType: %d",
				 (int) parsetree->commandType);
	}

	/*
	 * Check if there's a FOR [KEY] UPDATE/SHARE clause applying to this view.
	 *
	 * Note: we needn't explicitly consider any such clauses appearing in
	 * ancestor query levels; their effects have already been pushed down to
	 * here by markQueryForLocking, and will be reflected in "rc".
	 */
	rc = get_parse_rowmark(parsetree, rt_index);

	/*
	 * Make a modifiable copy of the view query, and acquire needed locks on
	 * the relations it mentions.  Force at least RowShareLock for all such
	 * rels if there's a FOR [KEY] UPDATE/SHARE clause affecting this view.
	 */
	rule_action = copyObject(linitial(rule->actions));

	AcquireRewriteLocks(rule_action, true, (rc != NULL));

	/*
	 * If FOR [KEY] UPDATE/SHARE of view, mark all the contained tables as
	 * implicit FOR [KEY] UPDATE/SHARE, the same as the parser would have done
	 * if the view's subquery had been written out explicitly.
	 */
	if (rc != NULL)
		markQueryForLocking(rule_action, (Node *) rule_action->jointree,
							rc->strength, rc->waitPolicy, true);

	/*
	 * Recursively expand any view references inside the view.
	 */
	rule_action = fireRIRrules(rule_action, activeRIRs);

	/*
	 * Make sure the query is marked as having row security if the view query
	 * does.
	 */
	parsetree->hasRowSecurity |= rule_action->hasRowSecurity;

	/*
	 * Now, plug the view query in as a subselect, converting the relation's
	 * original RTE to a subquery RTE.
	 */
	rte = rt_fetch(rt_index, parsetree->rtable);

	rte->rtekind = RTE_SUBQUERY;
	rte->subquery = rule_action;
	rte->security_barrier = RelationIsSecurityView(relation);

	/*
	 * Clear fields that should not be set in a subquery RTE.  Note that we
	 * leave the relid, relkind, rellockmode, and perminfoindex fields set, so
	 * that the view relation can be appropriately locked before execution and
	 * its permissions checked.
	 */
	rte->tablesample = NULL;
	rte->inh = false;			/* must not be set for a subquery */

	/*
	 * Since we allow CREATE OR REPLACE VIEW to add columns to a view, the
	 * rule_action might emit more columns than we expected when the current
	 * query was parsed.  Various places expect rte->eref->colnames to be
	 * consistent with the non-junk output columns of the subquery, so patch
	 * things up if necessary by adding some dummy column names.
	 */
	numCols = ExecCleanTargetListLength(rule_action->targetList);
	while (list_length(rte->eref->colnames) < numCols)
	{
		rte->eref->colnames = lappend(rte->eref->colnames,
									  makeString(pstrdup("?column?")));
	}

	return parsetree;
}

/*
 * Recursively mark all relations used by a view as FOR [KEY] UPDATE/SHARE.
 *
 * This may generate an invalid query, eg if some sub-query uses an
 * aggregate.  We leave it to the planner to detect that.
 *
 * NB: this must agree with the parser's transformLockingClause() routine.
 * However, we used to have to avoid marking a view's OLD and NEW rels for
 * updating, which motivated scanning the jointree to determine which rels
 * are used.  Possibly that could now be simplified into just scanning the
 * rangetable as the parser does.
 */
static void
markQueryForLocking(Query *qry, Node *jtnode,
					LockClauseStrength strength, LockWaitPolicy waitPolicy,
					bool pushedDown)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		int			rti = ((RangeTblRef *) jtnode)->rtindex;
		RangeTblEntry *rte = rt_fetch(rti, qry->rtable);

		if (rte->rtekind == RTE_RELATION)
		{
			RTEPermissionInfo *perminfo;

			applyLockingClause(qry, rti, strength, waitPolicy, pushedDown);

			perminfo = getRTEPermissionInfo(qry->rteperminfos, rte);
			perminfo->requiredPerms |= ACL_SELECT_FOR_UPDATE;
		}
		else if (rte->rtekind == RTE_SUBQUERY)
		{
			applyLockingClause(qry, rti, strength, waitPolicy, pushedDown);
			/* FOR UPDATE/SHARE of subquery is propagated to subquery's rels */
			markQueryForLocking(rte->subquery, (Node *) rte->subquery->jointree,
								strength, waitPolicy, true);
		}
		/* other RTE types are unaffected by FOR UPDATE */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
			markQueryForLocking(qry, lfirst(l), strength, waitPolicy, pushedDown);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		markQueryForLocking(qry, j->larg, strength, waitPolicy, pushedDown);
		markQueryForLocking(qry, j->rarg, strength, waitPolicy, pushedDown);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}


/*
 * fireRIRonSubLink -
 *	Apply fireRIRrules() to each SubLink (subselect in expression) found
 *	in the given tree.
 *
 * NOTE: although this has the form of a walker, we cheat and modify the
 * SubLink nodes in-place.  It is caller's responsibility to ensure that
 * no unwanted side-effects occur!
 *
 * This is unlike most of the other routines that recurse into subselects,
 * because we must take control at the SubLink node in order to replace
 * the SubLink's subselect link with the possibly-rewritten subquery.
 */
static bool
fireRIRonSubLink(Node *node, fireRIRonSubLink_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
	{
		SubLink    *sub = (SubLink *) node;

		/* Do what we came for */
		sub->subselect = (Node *) fireRIRrules((Query *) sub->subselect,
											   context->activeRIRs);

		/*
		 * Remember if any of the sublinks have row security.
		 */
		context->hasRowSecurity |= ((Query *) sub->subselect)->hasRowSecurity;

		/* Fall through to process lefthand args of SubLink */
	}

	/*
	 * Do NOT recurse into Query nodes, because fireRIRrules already processed
	 * subselects of subselects for us.
	 */
	return expression_tree_walker(node, fireRIRonSubLink, context);
}


/*
 * fireRIRrules -
 *	Apply all RIR rules on each rangetable entry in the given query
 *
 * activeRIRs is a list of the OIDs of views we're already processing RIR
 * rules for, used to detect/reject recursion.
 */
static Query *
fireRIRrules(Query *parsetree, List *activeRIRs)
{
	int			origResultRelation = parsetree->resultRelation;
	int			rt_index;
	ListCell   *lc;

	/*
	 * Expand SEARCH and CYCLE clauses in CTEs.
	 *
	 * This is just a convenient place to do this, since we are already
	 * looking at each Query.
	 */
	foreach(lc, parsetree->cteList)
	{
		CommonTableExpr *cte = lfirst_node(CommonTableExpr, lc);

		if (cte->search_clause || cte->cycle_clause)
		{
			cte = rewriteSearchAndCycle(cte);
			lfirst(lc) = cte;
		}
	}

	/*
	 * don't try to convert this into a foreach loop, because rtable list can
	 * get changed each time through...
	 */
	rt_index = 0;
	while (rt_index < list_length(parsetree->rtable))
	{
		RangeTblEntry *rte;
		Relation	rel;
		List	   *locks;
		RuleLock   *rules;
		RewriteRule *rule;
		int			i;

		++rt_index;

		rte = rt_fetch(rt_index, parsetree->rtable);

		/*
		 * A subquery RTE can't have associated rules, so there's nothing to
		 * do to this level of the query, but we must recurse into the
		 * subquery to expand any rule references in it.
		 */
		if (rte->rtekind == RTE_SUBQUERY)
		{
			rte->subquery = fireRIRrules(rte->subquery, activeRIRs);

			/*
			 * While we are here, make sure the query is marked as having row
			 * security if any of its subqueries do.
			 */
			parsetree->hasRowSecurity |= rte->subquery->hasRowSecurity;

			continue;
		}

		/*
		 * Joins and other non-relation RTEs can be ignored completely.
		 */
		if (rte->rtekind != RTE_RELATION)
			continue;

		/*
		 * Always ignore RIR rules for materialized views referenced in
		 * queries.  (This does not prevent refreshing MVs, since they aren't
		 * referenced in their own query definitions.)
		 *
		 * Note: in the future we might want to allow MVs to be conditionally
		 * expanded as if they were regular views, if they are not scannable.
		 * In that case this test would need to be postponed till after we've
		 * opened the rel, so that we could check its state.
		 */
		if (rte->relkind == RELKIND_MATVIEW)
			continue;

		/*
		 * In INSERT ... ON CONFLICT, ignore the EXCLUDED pseudo-relation;
		 * even if it points to a view, we needn't expand it, and should not
		 * because we want the RTE to remain of RTE_RELATION type.  Otherwise,
		 * it would get changed to RTE_SUBQUERY type, which is an
		 * untested/unsupported situation.
		 */
		if (parsetree->onConflict &&
			rt_index == parsetree->onConflict->exclRelIndex)
			continue;

		/*
		 * If the table is not referenced in the query, then we ignore it.
		 * This prevents infinite expansion loop due to new rtable entries
		 * inserted by expansion of a rule. A table is referenced if it is
		 * part of the join set (a source table), or is referenced by any Var
		 * nodes, or is the result table.
		 */
		if (rt_index != parsetree->resultRelation &&
			!rangeTableEntry_used((Node *) parsetree, rt_index, 0))
			continue;

		/*
		 * Also, if this is a new result relation introduced by
		 * ApplyRetrieveRule, we don't want to do anything more with it.
		 */
		if (rt_index == parsetree->resultRelation &&
			rt_index != origResultRelation)
			continue;

		/*
		 * We can use NoLock here since either the parser or
		 * AcquireRewriteLocks should have locked the rel already.
		 */
		rel = table_open(rte->relid, NoLock);

		/*
		 * Collect the RIR rules that we must apply
		 */
		rules = rel->rd_rules;
		if (rules != NULL)
		{
			locks = NIL;
			for (i = 0; i < rules->numLocks; i++)
			{
				rule = rules->rules[i];
				if (rule->event != CMD_SELECT)
					continue;

				locks = lappend(locks, rule);
			}

			/*
			 * If we found any, apply them --- but first check for recursion!
			 */
			if (locks != NIL)
			{
				ListCell   *l;

				if (list_member_oid(activeRIRs, RelationGetRelid(rel)))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("infinite recursion detected in rules for relation \"%s\"",
									RelationGetRelationName(rel))));
				activeRIRs = lappend_oid(activeRIRs, RelationGetRelid(rel));

				foreach(l, locks)
				{
					rule = lfirst(l);

					parsetree = ApplyRetrieveRule(parsetree,
												  rule,
												  rt_index,
												  rel,
												  activeRIRs);
				}

				activeRIRs = list_delete_last(activeRIRs);
			}
		}

		table_close(rel, NoLock);
	}

	/* Recurse into subqueries in WITH */
	foreach(lc, parsetree->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		cte->ctequery = (Node *)
			fireRIRrules((Query *) cte->ctequery, activeRIRs);

		/*
		 * While we are here, make sure the query is marked as having row
		 * security if any of its CTEs do.
		 */
		parsetree->hasRowSecurity |= ((Query *) cte->ctequery)->hasRowSecurity;
	}

	/*
	 * Recurse into sublink subqueries, too.  But we already did the ones in
	 * the rtable and cteList.
	 */
	if (parsetree->hasSubLinks)
	{
		fireRIRonSubLink_context context;

		context.activeRIRs = activeRIRs;
		context.hasRowSecurity = false;

		query_tree_walker(parsetree, fireRIRonSubLink, &context,
						  QTW_IGNORE_RC_SUBQUERIES);

		/*
		 * Make sure the query is marked as having row security if any of its
		 * sublinks do.
		 */
		parsetree->hasRowSecurity |= context.hasRowSecurity;
	}

	/*
	 * Apply any row-level security policies.  We do this last because it
	 * requires special recursion detection if the new quals have sublink
	 * subqueries, and if we did it in the loop above query_tree_walker would
	 * then recurse into those quals a second time.
	 */
	rt_index = 0;
	foreach(lc, parsetree->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
		Relation	rel;
		List	   *securityQuals;
		List	   *withCheckOptions;
		bool		hasRowSecurity;
		bool		hasSubLinks;

		++rt_index;

		/* Only normal relations can have RLS policies */
		if (rte->rtekind != RTE_RELATION ||
			(rte->relkind != RELKIND_RELATION &&
			 rte->relkind != RELKIND_PARTITIONED_TABLE))
			continue;

		rel = table_open(rte->relid, NoLock);

		/*
		 * Fetch any new security quals that must be applied to this RTE.
		 */
		get_row_security_policies(parsetree, rte, rt_index,
								  &securityQuals, &withCheckOptions,
								  &hasRowSecurity, &hasSubLinks);

		if (securityQuals != NIL || withCheckOptions != NIL)
		{
			if (hasSubLinks)
			{
				acquireLocksOnSubLinks_context context;
				fireRIRonSubLink_context fire_context;

				/*
				 * Recursively process the new quals, checking for infinite
				 * recursion.
				 */
				if (list_member_oid(activeRIRs, RelationGetRelid(rel)))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("infinite recursion detected in policy for relation \"%s\"",
									RelationGetRelationName(rel))));

				activeRIRs = lappend_oid(activeRIRs, RelationGetRelid(rel));

				/*
				 * get_row_security_policies just passed back securityQuals
				 * and/or withCheckOptions, and there were SubLinks, make sure
				 * we lock any relations which are referenced.
				 *
				 * These locks would normally be acquired by the parser, but
				 * securityQuals and withCheckOptions are added post-parsing.
				 */
				context.for_execute = true;
				(void) acquireLocksOnSubLinks((Node *) securityQuals, &context);
				(void) acquireLocksOnSubLinks((Node *) withCheckOptions,
											  &context);

				/*
				 * Now that we have the locks on anything added by
				 * get_row_security_policies, fire any RIR rules for them.
				 */
				fire_context.activeRIRs = activeRIRs;
				fire_context.hasRowSecurity = false;

				expression_tree_walker((Node *) securityQuals,
									   fireRIRonSubLink, &fire_context);

				expression_tree_walker((Node *) withCheckOptions,
									   fireRIRonSubLink, &fire_context);

				/*
				 * We can ignore the value of fire_context.hasRowSecurity
				 * since we only reach this code in cases where hasRowSecurity
				 * is already true.
				 */
				Assert(hasRowSecurity);

				activeRIRs = list_delete_last(activeRIRs);
			}

			/*
			 * Add the new security barrier quals to the start of the RTE's
			 * list so that they get applied before any existing barrier quals
			 * (which would have come from a security-barrier view, and should
			 * get lower priority than RLS conditions on the table itself).
			 */
			rte->securityQuals = list_concat(securityQuals,
											 rte->securityQuals);

			parsetree->withCheckOptions = list_concat(withCheckOptions,
													  parsetree->withCheckOptions);
		}

		/*
		 * Make sure the query is marked correctly if row-level security
		 * applies, or if the new quals had sublinks.
		 */
		if (hasRowSecurity)
			parsetree->hasRowSecurity = true;
		if (hasSubLinks)
			parsetree->hasSubLinks = true;

		table_close(rel, NoLock);
	}

	return parsetree;
}


/*
 * Modify the given query by adding 'AND rule_qual IS NOT TRUE' to its
 * qualification.  This is used to generate suitable "else clauses" for
 * conditional INSTEAD rules.  (Unfortunately we must use "x IS NOT TRUE",
 * not just "NOT x" which the planner is much smarter about, else we will
 * do the wrong thing when the qual evaluates to NULL.)
 *
 * The rule_qual may contain references to OLD or NEW.  OLD references are
 * replaced by references to the specified rt_index (the relation that the
 * rule applies to).  NEW references are only possible for INSERT and UPDATE
 * queries on the relation itself, and so they should be replaced by copies
 * of the related entries in the query's own targetlist.
 */
static Query *
CopyAndAddInvertedQual(Query *parsetree,
					   Node *rule_qual,
					   int rt_index,
					   CmdType event)
{
	/* Don't scribble on the passed qual (it's in the relcache!) */
	Node	   *new_qual = copyObject(rule_qual);
	acquireLocksOnSubLinks_context context;

	context.for_execute = true;

	/*
	 * In case there are subqueries in the qual, acquire necessary locks and
	 * fix any deleted JOIN RTE entries.  (This is somewhat redundant with
	 * rewriteRuleAction, but not entirely ... consider restructuring so that
	 * we only need to process the qual this way once.)
	 */
	(void) acquireLocksOnSubLinks(new_qual, &context);

	/* Fix references to OLD */
	ChangeVarNodes(new_qual, PRS2_OLD_VARNO, rt_index, 0);
	/* Fix references to NEW */
	if (event == CMD_INSERT || event == CMD_UPDATE)
		new_qual = ReplaceVarsFromTargetList(new_qual,
											 PRS2_NEW_VARNO,
											 0,
											 rt_fetch(rt_index,
													  parsetree->rtable),
											 parsetree->targetList,
											 parsetree->resultRelation,
											 (event == CMD_UPDATE) ?
											 REPLACEVARS_CHANGE_VARNO :
											 REPLACEVARS_SUBSTITUTE_NULL,
											 rt_index,
											 &parsetree->hasSubLinks);
	/* And attach the fixed qual */
	AddInvertedQual(parsetree, new_qual);

	return parsetree;
}


/*
 *	fireRules -
 *	   Iterate through rule locks applying rules.
 *
 * Input arguments:
 *	parsetree - original query
 *	rt_index - RT index of result relation in original query
 *	event - type of rule event
 *	locks - list of rules to fire
 * Output arguments:
 *	*instead_flag - set true if any unqualified INSTEAD rule is found
 *					(must be initialized to false)
 *	*returning_flag - set true if we rewrite RETURNING clause in any rule
 *					(must be initialized to false)
 *	*qual_product - filled with modified original query if any qualified
 *					INSTEAD rule is found (must be initialized to NULL)
 * Return value:
 *	list of rule actions adjusted for use with this query
 *
 * Qualified INSTEAD rules generate their action with the qualification
 * condition added.  They also generate a modified version of the original
 * query with the negated qualification added, so that it will run only for
 * rows that the qualified action doesn't act on.  (If there are multiple
 * qualified INSTEAD rules, we AND all the negated quals onto a single
 * modified original query.)  We won't execute the original, unmodified
 * query if we find either qualified or unqualified INSTEAD rules.  If
 * we find both, the modified original query is discarded too.
 */
static List *
fireRules(Query *parsetree,
		  int rt_index,
		  CmdType event,
		  List *locks,
		  bool *instead_flag,
		  bool *returning_flag,
		  Query **qual_product)
{
	List	   *results = NIL;
	ListCell   *l;

	foreach(l, locks)
	{
		RewriteRule *rule_lock = (RewriteRule *) lfirst(l);
		Node	   *event_qual = rule_lock->qual;
		List	   *actions = rule_lock->actions;
		QuerySource qsrc;
		ListCell   *r;

		/* Determine correct QuerySource value for actions */
		if (rule_lock->isInstead)
		{
			if (event_qual != NULL)
				qsrc = QSRC_QUAL_INSTEAD_RULE;
			else
			{
				qsrc = QSRC_INSTEAD_RULE;
				*instead_flag = true;	/* report unqualified INSTEAD */
			}
		}
		else
			qsrc = QSRC_NON_INSTEAD_RULE;

		if (qsrc == QSRC_QUAL_INSTEAD_RULE)
		{
			/*
			 * If there are INSTEAD rules with qualifications, the original
			 * query is still performed. But all the negated rule
			 * qualifications of the INSTEAD rules are added so it does its
			 * actions only in cases where the rule quals of all INSTEAD rules
			 * are false. Think of it as the default action in a case. We save
			 * this in *qual_product so RewriteQuery() can add it to the query
			 * list after we mangled it up enough.
			 *
			 * If we have already found an unqualified INSTEAD rule, then
			 * *qual_product won't be used, so don't bother building it.
			 */
			if (!*instead_flag)
			{
				if (*qual_product == NULL)
					*qual_product = copyObject(parsetree);
				*qual_product = CopyAndAddInvertedQual(*qual_product,
													   event_qual,
													   rt_index,
													   event);
			}
		}

		/* Now process the rule's actions and add them to the result list */
		foreach(r, actions)
		{
			Query	   *rule_action = lfirst(r);

			if (rule_action->commandType == CMD_NOTHING)
				continue;

			rule_action = rewriteRuleAction(parsetree, rule_action,
											event_qual, rt_index, event,
											returning_flag);

			rule_action->querySource = qsrc;
			rule_action->canSetTag = false; /* might change later */

			results = lappend(results, rule_action);
		}
	}

	return results;
}


/*
 * get_view_query - get the Query from a view's _RETURN rule.
 *
 * Caller should have verified that the relation is a view, and therefore
 * we should find an ON SELECT action.
 *
 * Note that the pointer returned is into the relcache and therefore must
 * be treated as read-only to the caller and not modified or scribbled on.
 */
Query *
get_view_query(Relation view)
{
	int			i;

	Assert(view->rd_rel->relkind == RELKIND_VIEW);

	for (i = 0; i < view->rd_rules->numLocks; i++)
	{
		RewriteRule *rule = view->rd_rules->rules[i];

		if (rule->event == CMD_SELECT)
		{
			/* A _RETURN rule should have only one action */
			if (list_length(rule->actions) != 1)
				elog(ERROR, "invalid _RETURN rule action specification");

			return (Query *) linitial(rule->actions);
		}
	}

	elog(ERROR, "failed to find _RETURN rule for view");
	return NULL;				/* keep compiler quiet */
}


/*
 * view_has_instead_trigger - does view have an INSTEAD OF trigger for event?
 *
 * If it does, we don't want to treat it as auto-updatable.  This test can't
 * be folded into view_query_is_auto_updatable because it's not an error
 * condition.
 *
 * For MERGE, this will return true if there is an INSTEAD OF trigger for
 * every action in mergeActionList, and false if there are any actions that
 * lack an INSTEAD OF trigger.  If there are no data-modifying MERGE actions
 * (only DO NOTHING actions), true is returned so that the view is treated
 * as trigger-updatable, rather than erroring out if it's not auto-updatable.
 */
bool
view_has_instead_trigger(Relation view, CmdType event, List *mergeActionList)
{
	TriggerDesc *trigDesc = view->trigdesc;

	switch (event)
	{
		case CMD_INSERT:
			if (trigDesc && trigDesc->trig_insert_instead_row)
				return true;
			break;
		case CMD_UPDATE:
			if (trigDesc && trigDesc->trig_update_instead_row)
				return true;
			break;
		case CMD_DELETE:
			if (trigDesc && trigDesc->trig_delete_instead_row)
				return true;
			break;
		case CMD_MERGE:
			foreach_node(MergeAction, action, mergeActionList)
			{
				switch (action->commandType)
				{
					case CMD_INSERT:
						if (!trigDesc || !trigDesc->trig_insert_instead_row)
							return false;
						break;
					case CMD_UPDATE:
						if (!trigDesc || !trigDesc->trig_update_instead_row)
							return false;
						break;
					case CMD_DELETE:
						if (!trigDesc || !trigDesc->trig_delete_instead_row)
							return false;
						break;
					case CMD_NOTHING:
						/* No trigger required */
						break;
					default:
						elog(ERROR, "unrecognized commandType: %d", action->commandType);
						break;
				}
			}
			return true;		/* no actions without an INSTEAD OF trigger */
		default:
			elog(ERROR, "unrecognized CmdType: %d", (int) event);
			break;
	}
	return false;
}


/*
 * view_col_is_auto_updatable - test whether the specified column of a view
 * is auto-updatable. Returns NULL (if the column can be updated) or a message
 * string giving the reason that it cannot be.
 *
 * The returned string has not been translated; if it is shown as an error
 * message, the caller should apply _() to translate it.
 *
 * Note that the checks performed here are local to this view. We do not check
 * whether the referenced column of the underlying base relation is updatable.
 */
static const char *
view_col_is_auto_updatable(RangeTblRef *rtr, TargetEntry *tle)
{
	Var		   *var = (Var *) tle->expr;

	/*
	 * For now, the only updatable columns we support are those that are Vars
	 * referring to user columns of the underlying base relation.
	 *
	 * The view targetlist may contain resjunk columns (e.g., a view defined
	 * like "SELECT * FROM t ORDER BY a+b" is auto-updatable) but such columns
	 * are not auto-updatable, and in fact should never appear in the outer
	 * query's targetlist.
	 */
	if (tle->resjunk)
		return gettext_noop("Junk view columns are not updatable.");

	if (!IsA(var, Var) ||
		var->varno != rtr->rtindex ||
		var->varlevelsup != 0)
		return gettext_noop("View columns that are not columns of their base relation are not updatable.");

	if (var->varattno < 0)
		return gettext_noop("View columns that refer to system columns are not updatable.");

	if (var->varattno == 0)
		return gettext_noop("View columns that return whole-row references are not updatable.");

	return NULL;				/* the view column is updatable */
}


/*
 * view_query_is_auto_updatable - test whether the specified view definition
 * represents an auto-updatable view. Returns NULL (if the view can be updated)
 * or a message string giving the reason that it cannot be.

 * The returned string has not been translated; if it is shown as an error
 * message, the caller should apply _() to translate it.
 *
 * If check_cols is true, the view is required to have at least one updatable
 * column (necessary for INSERT/UPDATE). Otherwise the view's columns are not
 * checked for updatability. See also view_cols_are_auto_updatable.
 *
 * Note that the checks performed here are only based on the view definition.
 * We do not check whether any base relations referred to by the view are
 * updatable.
 */
const char *
view_query_is_auto_updatable(Query *viewquery, bool check_cols)
{
	RangeTblRef *rtr;
	RangeTblEntry *base_rte;

	/*----------
	 * Check if the view is simply updatable.  According to SQL-92 this means:
	 *	- No DISTINCT clause.
	 *	- Each TLE is a column reference, and each column appears at most once.
	 *	- FROM contains exactly one base relation.
	 *	- No GROUP BY or HAVING clauses.
	 *	- No set operations (UNION, INTERSECT or EXCEPT).
	 *	- No sub-queries in the WHERE clause that reference the target table.
	 *
	 * We ignore that last restriction since it would be complex to enforce
	 * and there isn't any actual benefit to disallowing sub-queries.  (The
	 * semantic issues that the standard is presumably concerned about don't
	 * arise in Postgres, since any such sub-query will not see any updates
	 * executed by the outer query anyway, thanks to MVCC snapshotting.)
	 *
	 * We also relax the second restriction by supporting part of SQL:1999
	 * feature T111, which allows for a mix of updatable and non-updatable
	 * columns, provided that an INSERT or UPDATE doesn't attempt to assign to
	 * a non-updatable column.
	 *
	 * In addition we impose these constraints, involving features that are
	 * not part of SQL-92:
	 *	- No CTEs (WITH clauses).
	 *	- No OFFSET or LIMIT clauses (this matches a SQL:2008 restriction).
	 *	- No system columns (including whole-row references) in the tlist.
	 *	- No window functions in the tlist.
	 *	- No set-returning functions in the tlist.
	 *
	 * Note that we do these checks without recursively expanding the view.
	 * If the base relation is a view, we'll recursively deal with it later.
	 *----------
	 */
	if (viewquery->distinctClause != NIL)
		return gettext_noop("Views containing DISTINCT are not automatically updatable.");

	if (viewquery->groupClause != NIL || viewquery->groupingSets)
		return gettext_noop("Views containing GROUP BY are not automatically updatable.");

	if (viewquery->havingQual != NULL)
		return gettext_noop("Views containing HAVING are not automatically updatable.");

	if (viewquery->setOperations != NULL)
		return gettext_noop("Views containing UNION, INTERSECT, or EXCEPT are not automatically updatable.");

	if (viewquery->cteList != NIL)
		return gettext_noop("Views containing WITH are not automatically updatable.");

	if (viewquery->limitOffset != NULL || viewquery->limitCount != NULL)
		return gettext_noop("Views containing LIMIT or OFFSET are not automatically updatable.");

	/*
	 * We must not allow window functions or set returning functions in the
	 * targetlist. Otherwise we might end up inserting them into the quals of
	 * the main query. We must also check for aggregates in the targetlist in
	 * case they appear without a GROUP BY.
	 *
	 * These restrictions ensure that each row of the view corresponds to a
	 * unique row in the underlying base relation.
	 */
	if (viewquery->hasAggs)
		return gettext_noop("Views that return aggregate functions are not automatically updatable.");

	if (viewquery->hasWindowFuncs)
		return gettext_noop("Views that return window functions are not automatically updatable.");

	if (viewquery->hasTargetSRFs)
		return gettext_noop("Views that return set-returning functions are not automatically updatable.");

	/*
	 * The view query should select from a single base relation, which must be
	 * a table or another view.
	 */
	if (list_length(viewquery->jointree->fromlist) != 1)
		return gettext_noop("Views that do not select from a single table or view are not automatically updatable.");

	rtr = (RangeTblRef *) linitial(viewquery->jointree->fromlist);
	if (!IsA(rtr, RangeTblRef))
		return gettext_noop("Views that do not select from a single table or view are not automatically updatable.");

	base_rte = rt_fetch(rtr->rtindex, viewquery->rtable);
	if (base_rte->rtekind != RTE_RELATION ||
		(base_rte->relkind != RELKIND_RELATION &&
		 base_rte->relkind != RELKIND_FOREIGN_TABLE &&
		 base_rte->relkind != RELKIND_VIEW &&
		 base_rte->relkind != RELKIND_PARTITIONED_TABLE))
		return gettext_noop("Views that do not select from a single table or view are not automatically updatable.");

	if (base_rte->tablesample)
		return gettext_noop("Views containing TABLESAMPLE are not automatically updatable.");

	/*
	 * Check that the view has at least one updatable column. This is required
	 * for INSERT/UPDATE but not for DELETE.
	 */
	if (check_cols)
	{
		ListCell   *cell;
		bool		found;

		found = false;
		foreach(cell, viewquery->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(cell);

			if (view_col_is_auto_updatable(rtr, tle) == NULL)
			{
				found = true;
				break;
			}
		}

		if (!found)
			return gettext_noop("Views that have no updatable columns are not automatically updatable.");
	}

	return NULL;				/* the view is updatable */
}


/*
 * view_cols_are_auto_updatable - test whether all of the required columns of
 * an auto-updatable view are actually updatable. Returns NULL (if all the
 * required columns can be updated) or a message string giving the reason that
 * they cannot be.
 *
 * The returned string has not been translated; if it is shown as an error
 * message, the caller should apply _() to translate it.
 *
 * This should be used for INSERT/UPDATE to ensure that we don't attempt to
 * assign to any non-updatable columns.
 *
 * Additionally it may be used to retrieve the set of updatable columns in the
 * view, or if one or more of the required columns is not updatable, the name
 * of the first offending non-updatable column.
 *
 * The caller must have already verified that this is an auto-updatable view
 * using view_query_is_auto_updatable.
 *
 * Note that the checks performed here are only based on the view definition.
 * We do not check whether the referenced columns of the base relation are
 * updatable.
 */
static const char *
view_cols_are_auto_updatable(Query *viewquery,
							 Bitmapset *required_cols,
							 Bitmapset **updatable_cols,
							 char **non_updatable_col)
{
	RangeTblRef *rtr;
	AttrNumber	col;
	ListCell   *cell;

	/*
	 * The caller should have verified that this view is auto-updatable and so
	 * there should be a single base relation.
	 */
	Assert(list_length(viewquery->jointree->fromlist) == 1);
	rtr = linitial_node(RangeTblRef, viewquery->jointree->fromlist);

	/* Initialize the optional return values */
	if (updatable_cols != NULL)
		*updatable_cols = NULL;
	if (non_updatable_col != NULL)
		*non_updatable_col = NULL;

	/* Test each view column for updatability */
	col = -FirstLowInvalidHeapAttributeNumber;
	foreach(cell, viewquery->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(cell);
		const char *col_update_detail;

		col++;
		col_update_detail = view_col_is_auto_updatable(rtr, tle);

		if (col_update_detail == NULL)
		{
			/* The column is updatable */
			if (updatable_cols != NULL)
				*updatable_cols = bms_add_member(*updatable_cols, col);
		}
		else if (bms_is_member(col, required_cols))
		{
			/* The required column is not updatable */
			if (non_updatable_col != NULL)
				*non_updatable_col = tle->resname;
			return col_update_detail;
		}
	}

	return NULL;				/* all the required view columns are updatable */
}


/*
 * relation_is_updatable - determine which update events the specified
 * relation supports.
 *
 * Note that views may contain a mix of updatable and non-updatable columns.
 * For a view to support INSERT/UPDATE it must have at least one updatable
 * column, but there is no such restriction for DELETE. If include_cols is
 * non-NULL, then only the specified columns are considered when testing for
 * updatability.
 *
 * Unlike the preceding functions, this does recurse to look at a view's
 * base relations, so it needs to detect recursion.  To do that, we pass
 * a list of currently-considered outer relations.  External callers need
 * only pass NIL.
 *
 * This is used for the information_schema views, which have separate concepts
 * of "updatable" and "trigger updatable".  A relation is "updatable" if it
 * can be updated without the need for triggers (either because it has a
 * suitable RULE, or because it is simple enough to be automatically updated).
 * A relation is "trigger updatable" if it has a suitable INSTEAD OF trigger.
 * The SQL standard regards this as not necessarily updatable, presumably
 * because there is no way of knowing what the trigger will actually do.
 * The information_schema views therefore call this function with
 * include_triggers = false.  However, other callers might only care whether
 * data-modifying SQL will work, so they can pass include_triggers = true
 * to have trigger updatability included in the result.
 *
 * The return value is a bitmask of rule event numbers indicating which of
 * the INSERT, UPDATE and DELETE operations are supported.  (We do it this way
 * so that we can test for UPDATE plus DELETE support in a single call.)
 */
int
relation_is_updatable(Oid reloid,
					  List *outer_reloids,
					  bool include_triggers,
					  Bitmapset *include_cols)
{
	int			events = 0;
	Relation	rel;
	RuleLock   *rulelocks;

#define ALL_EVENTS ((1 << CMD_INSERT) | (1 << CMD_UPDATE) | (1 << CMD_DELETE))

	/* Since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	rel = try_relation_open(reloid, AccessShareLock);

	/*
	 * If the relation doesn't exist, return zero rather than throwing an
	 * error.  This is helpful since scanning an information_schema view under
	 * MVCC rules can result in referencing rels that have actually been
	 * deleted already.
	 */
	if (rel == NULL)
		return 0;

	/* If we detect a recursive view, report that it is not updatable */
	if (list_member_oid(outer_reloids, RelationGetRelid(rel)))
	{
		relation_close(rel, AccessShareLock);
		return 0;
	}

	/* If the relation is a table, it is always updatable */
	if (rel->rd_rel->relkind == RELKIND_RELATION ||
		rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		relation_close(rel, AccessShareLock);
		return ALL_EVENTS;
	}

	/* Look for unconditional DO INSTEAD rules, and note supported events */
	rulelocks = rel->rd_rules;
	if (rulelocks != NULL)
	{
		int			i;

		for (i = 0; i < rulelocks->numLocks; i++)
		{
			if (rulelocks->rules[i]->isInstead &&
				rulelocks->rules[i]->qual == NULL)
			{
				events |= ((1 << rulelocks->rules[i]->event) & ALL_EVENTS);
			}
		}

		/* If we have rules for all events, we're done */
		if (events == ALL_EVENTS)
		{
			relation_close(rel, AccessShareLock);
			return events;
		}
	}

	/* Similarly look for INSTEAD OF triggers, if they are to be included */
	if (include_triggers)
	{
		TriggerDesc *trigDesc = rel->trigdesc;

		if (trigDesc)
		{
			if (trigDesc->trig_insert_instead_row)
				events |= (1 << CMD_INSERT);
			if (trigDesc->trig_update_instead_row)
				events |= (1 << CMD_UPDATE);
			if (trigDesc->trig_delete_instead_row)
				events |= (1 << CMD_DELETE);

			/* If we have triggers for all events, we're done */
			if (events == ALL_EVENTS)
			{
				relation_close(rel, AccessShareLock);
				return events;
			}
		}
	}

	/* If this is a foreign table, check which update events it supports */
	if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		FdwRoutine *fdwroutine = GetFdwRoutineForRelation(rel, false);

		if (fdwroutine->IsForeignRelUpdatable != NULL)
			events |= fdwroutine->IsForeignRelUpdatable(rel);
		else
		{
			/* Assume presence of executor functions is sufficient */
			if (fdwroutine->ExecForeignInsert != NULL)
				events |= (1 << CMD_INSERT);
			if (fdwroutine->ExecForeignUpdate != NULL)
				events |= (1 << CMD_UPDATE);
			if (fdwroutine->ExecForeignDelete != NULL)
				events |= (1 << CMD_DELETE);
		}

		relation_close(rel, AccessShareLock);
		return events;
	}

	/* Check if this is an automatically updatable view */
	if (rel->rd_rel->relkind == RELKIND_VIEW)
	{
		Query	   *viewquery = get_view_query(rel);

		if (view_query_is_auto_updatable(viewquery, false) == NULL)
		{
			Bitmapset  *updatable_cols;
			int			auto_events;
			RangeTblRef *rtr;
			RangeTblEntry *base_rte;
			Oid			baseoid;

			/*
			 * Determine which of the view's columns are updatable. If there
			 * are none within the set of columns we are looking at, then the
			 * view doesn't support INSERT/UPDATE, but it may still support
			 * DELETE.
			 */
			view_cols_are_auto_updatable(viewquery, NULL,
										 &updatable_cols, NULL);

			if (include_cols != NULL)
				updatable_cols = bms_int_members(updatable_cols, include_cols);

			if (bms_is_empty(updatable_cols))
				auto_events = (1 << CMD_DELETE);	/* May support DELETE */
			else
				auto_events = ALL_EVENTS;	/* May support all events */

			/*
			 * The base relation must also support these update commands.
			 * Tables are always updatable, but for any other kind of base
			 * relation we must do a recursive check limited to the columns
			 * referenced by the locally updatable columns in this view.
			 */
			rtr = (RangeTblRef *) linitial(viewquery->jointree->fromlist);
			base_rte = rt_fetch(rtr->rtindex, viewquery->rtable);
			Assert(base_rte->rtekind == RTE_RELATION);

			if (base_rte->relkind != RELKIND_RELATION &&
				base_rte->relkind != RELKIND_PARTITIONED_TABLE)
			{
				baseoid = base_rte->relid;
				outer_reloids = lappend_oid(outer_reloids,
											RelationGetRelid(rel));
				include_cols = adjust_view_column_set(updatable_cols,
													  viewquery->targetList);
				auto_events &= relation_is_updatable(baseoid,
													 outer_reloids,
													 include_triggers,
													 include_cols);
				outer_reloids = list_delete_last(outer_reloids);
			}
			events |= auto_events;
		}
	}

	/* If we reach here, the relation may support some update commands */
	relation_close(rel, AccessShareLock);
	return events;
}


/*
 * adjust_view_column_set - map a set of column numbers according to targetlist
 *
 * This is used with simply-updatable views to map column-permissions sets for
 * the view columns onto the matching columns in the underlying base relation.
 * Relevant entries in the targetlist must be plain Vars of the underlying
 * relation (as per the checks above in view_query_is_auto_updatable).
 */
static Bitmapset *
adjust_view_column_set(Bitmapset *cols, List *targetlist)
{
	Bitmapset  *result = NULL;
	int			col;

	col = -1;
	while ((col = bms_next_member(cols, col)) >= 0)
	{
		/* bit numbers are offset by FirstLowInvalidHeapAttributeNumber */
		AttrNumber	attno = col + FirstLowInvalidHeapAttributeNumber;

		if (attno == InvalidAttrNumber)
		{
			/*
			 * There's a whole-row reference to the view.  For permissions
			 * purposes, treat it as a reference to each column available from
			 * the view.  (We should *not* convert this to a whole-row
			 * reference to the base relation, since the view may not touch
			 * all columns of the base relation.)
			 */
			ListCell   *lc;

			foreach(lc, targetlist)
			{
				TargetEntry *tle = lfirst_node(TargetEntry, lc);
				Var		   *var;

				if (tle->resjunk)
					continue;
				var = castNode(Var, tle->expr);
				result = bms_add_member(result,
										var->varattno - FirstLowInvalidHeapAttributeNumber);
			}
		}
		else
		{
			/*
			 * Views do not have system columns, so we do not expect to see
			 * any other system attnos here.  If we do find one, the error
			 * case will apply.
			 */
			TargetEntry *tle = get_tle_by_resno(targetlist, attno);

			if (tle != NULL && !tle->resjunk && IsA(tle->expr, Var))
			{
				Var		   *var = (Var *) tle->expr;

				result = bms_add_member(result,
										var->varattno - FirstLowInvalidHeapAttributeNumber);
			}
			else
				elog(ERROR, "attribute number %d not found in view targetlist",
					 attno);
		}
	}

	return result;
}


/*
 * error_view_not_updatable -
 *	  Report an error due to an attempt to update a non-updatable view.
 *
 * Generally this is expected to be called from the rewriter, with suitable
 * error detail explaining why the view is not updatable.  Note, however, that
 * the executor also performs a just-in-case check that the target view is
 * updatable.  That check is expected to never fail, but if it does, it will
 * call this function with NULL error detail --- see CheckValidResultRel().
 *
 * Note: for MERGE, at least one of the actions in mergeActionList is expected
 * to lack a suitable INSTEAD OF trigger --- see view_has_instead_trigger().
 */
void
error_view_not_updatable(Relation view,
						 CmdType command,
						 List *mergeActionList,
						 const char *detail)
{
	TriggerDesc *trigDesc = view->trigdesc;

	switch (command)
	{
		case CMD_INSERT:
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("cannot insert into view \"%s\"",
						   RelationGetRelationName(view)),
					detail ? errdetail_internal("%s", _(detail)) : 0,
					errhint("To enable inserting into the view, provide an INSTEAD OF INSERT trigger or an unconditional ON INSERT DO INSTEAD rule."));
			break;
		case CMD_UPDATE:
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("cannot update view \"%s\"",
						   RelationGetRelationName(view)),
					detail ? errdetail_internal("%s", _(detail)) : 0,
					errhint("To enable updating the view, provide an INSTEAD OF UPDATE trigger or an unconditional ON UPDATE DO INSTEAD rule."));
			break;
		case CMD_DELETE:
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("cannot delete from view \"%s\"",
						   RelationGetRelationName(view)),
					detail ? errdetail_internal("%s", _(detail)) : 0,
					errhint("To enable deleting from the view, provide an INSTEAD OF DELETE trigger or an unconditional ON DELETE DO INSTEAD rule."));
			break;
		case CMD_MERGE:

			/*
			 * Note that the error hints here differ from above, since MERGE
			 * doesn't support rules.
			 */
			foreach_node(MergeAction, action, mergeActionList)
			{
				switch (action->commandType)
				{
					case CMD_INSERT:
						if (!trigDesc || !trigDesc->trig_insert_instead_row)
							ereport(ERROR,
									errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
									errmsg("cannot insert into view \"%s\"",
										   RelationGetRelationName(view)),
									detail ? errdetail_internal("%s", _(detail)) : 0,
									errhint("To enable inserting into the view using MERGE, provide an INSTEAD OF INSERT trigger."));
						break;
					case CMD_UPDATE:
						if (!trigDesc || !trigDesc->trig_update_instead_row)
							ereport(ERROR,
									errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
									errmsg("cannot update view \"%s\"",
										   RelationGetRelationName(view)),
									detail ? errdetail_internal("%s", _(detail)) : 0,
									errhint("To enable updating the view using MERGE, provide an INSTEAD OF UPDATE trigger."));
						break;
					case CMD_DELETE:
						if (!trigDesc || !trigDesc->trig_delete_instead_row)
							ereport(ERROR,
									errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
									errmsg("cannot delete from view \"%s\"",
										   RelationGetRelationName(view)),
									detail ? errdetail_internal("%s", _(detail)) : 0,
									errhint("To enable deleting from the view using MERGE, provide an INSTEAD OF DELETE trigger."));
						break;
					case CMD_NOTHING:
						break;
					default:
						elog(ERROR, "unrecognized commandType: %d", action->commandType);
						break;
				}
			}
			break;
		default:
			elog(ERROR, "unrecognized CmdType: %d", (int) command);
			break;
	}
}


/*
 * rewriteTargetView -
 *	  Attempt to rewrite a query where the target relation is a view, so that
 *	  the view's base relation becomes the target relation.
 *
 * Note that the base relation here may itself be a view, which may or may not
 * have INSTEAD OF triggers or rules to handle the update.  That is handled by
 * the recursion in RewriteQuery.
 */
static Query *
rewriteTargetView(Query *parsetree, Relation view)
{
	Query	   *viewquery;
	bool		insert_or_update;
	const char *auto_update_detail;
	RangeTblRef *rtr;
	int			base_rt_index;
	int			new_rt_index;
	RangeTblEntry *base_rte;
	RangeTblEntry *view_rte;
	RangeTblEntry *new_rte;
	RTEPermissionInfo *base_perminfo;
	RTEPermissionInfo *view_perminfo;
	RTEPermissionInfo *new_perminfo;
	Relation	base_rel;
	List	   *view_targetlist;
	ListCell   *lc;

	/*
	 * Get the Query from the view's ON SELECT rule.  We're going to munge the
	 * Query to change the view's base relation into the target relation,
	 * along with various other changes along the way, so we need to make a
	 * copy of it (get_view_query() returns a pointer into the relcache, so we
	 * have to treat it as read-only).
	 */
	viewquery = copyObject(get_view_query(view));

	/* Locate RTE and perminfo describing the view in the outer query */
	view_rte = rt_fetch(parsetree->resultRelation, parsetree->rtable);
	view_perminfo = getRTEPermissionInfo(parsetree->rteperminfos, view_rte);

	/*
	 * Are we doing INSERT/UPDATE, or MERGE containing INSERT/UPDATE?  If so,
	 * various additional checks on the view columns need to be applied, and
	 * any view CHECK OPTIONs need to be enforced.
	 */
	insert_or_update =
		(parsetree->commandType == CMD_INSERT ||
		 parsetree->commandType == CMD_UPDATE);

	if (parsetree->commandType == CMD_MERGE)
	{
		foreach_node(MergeAction, action, parsetree->mergeActionList)
		{
			if (action->commandType == CMD_INSERT ||
				action->commandType == CMD_UPDATE)
			{
				insert_or_update = true;
				break;
			}
		}
	}

	/* Check if the expansion of non-system views are restricted */
	if (unlikely((restrict_nonsystem_relation_kind & RESTRICT_RELKIND_VIEW) != 0 &&
				 RelationGetRelid(view) >= FirstNormalObjectId))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("access to non-system view \"%s\" is restricted",
						RelationGetRelationName(view))));

	/*
	 * The view must be updatable, else fail.
	 *
	 * If we are doing INSERT/UPDATE (or MERGE containing INSERT/UPDATE), we
	 * also check that there is at least one updatable column.
	 */
	auto_update_detail =
		view_query_is_auto_updatable(viewquery, insert_or_update);

	if (auto_update_detail)
		error_view_not_updatable(view,
								 parsetree->commandType,
								 parsetree->mergeActionList,
								 auto_update_detail);

	/*
	 * For INSERT/UPDATE (or MERGE containing INSERT/UPDATE) the modified
	 * columns must all be updatable.
	 */
	if (insert_or_update)
	{
		Bitmapset  *modified_cols;
		char	   *non_updatable_col;

		/*
		 * Compute the set of modified columns as those listed in the result
		 * RTE's insertedCols and/or updatedCols sets plus those that are
		 * targets of the query's targetlist(s).  We must consider the query's
		 * targetlist because rewriteTargetListIU may have added additional
		 * targetlist entries for view defaults, and these must also be
		 * updatable.  But rewriteTargetListIU can also remove entries if they
		 * are DEFAULT markers and the column's default is NULL, so
		 * considering only the targetlist would also be wrong.
		 */
		modified_cols = bms_union(view_perminfo->insertedCols,
								  view_perminfo->updatedCols);

		foreach(lc, parsetree->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);

			if (!tle->resjunk)
				modified_cols = bms_add_member(modified_cols,
											   tle->resno - FirstLowInvalidHeapAttributeNumber);
		}

		if (parsetree->onConflict)
		{
			foreach(lc, parsetree->onConflict->onConflictSet)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(lc);

				if (!tle->resjunk)
					modified_cols = bms_add_member(modified_cols,
												   tle->resno - FirstLowInvalidHeapAttributeNumber);
			}
		}

		foreach_node(MergeAction, action, parsetree->mergeActionList)
		{
			if (action->commandType == CMD_INSERT ||
				action->commandType == CMD_UPDATE)
			{
				foreach_node(TargetEntry, tle, action->targetList)
				{
					if (!tle->resjunk)
						modified_cols = bms_add_member(modified_cols,
													   tle->resno - FirstLowInvalidHeapAttributeNumber);
				}
			}
		}

		auto_update_detail = view_cols_are_auto_updatable(viewquery,
														  modified_cols,
														  NULL,
														  &non_updatable_col);
		if (auto_update_detail)
		{
			/*
			 * This is a different error, caused by an attempt to update a
			 * non-updatable column in an otherwise updatable view.
			 */
			switch (parsetree->commandType)
			{
				case CMD_INSERT:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot insert into column \"%s\" of view \"%s\"",
									non_updatable_col,
									RelationGetRelationName(view)),
							 errdetail_internal("%s", _(auto_update_detail))));
					break;
				case CMD_UPDATE:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot update column \"%s\" of view \"%s\"",
									non_updatable_col,
									RelationGetRelationName(view)),
							 errdetail_internal("%s", _(auto_update_detail))));
					break;
				case CMD_MERGE:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot merge into column \"%s\" of view \"%s\"",
									non_updatable_col,
									RelationGetRelationName(view)),
							 errdetail_internal("%s", _(auto_update_detail))));
					break;
				default:
					elog(ERROR, "unrecognized CmdType: %d",
						 (int) parsetree->commandType);
					break;
			}
		}
	}

	/*
	 * For MERGE, there must not be any INSTEAD OF triggers on an otherwise
	 * updatable view.  The caller already checked that there isn't a full set
	 * of INSTEAD OF triggers, so this is to guard against having a partial
	 * set (mixing auto-update and trigger-update actions in a single command
	 * isn't supported).
	 */
	if (parsetree->commandType == CMD_MERGE)
	{
		foreach_node(MergeAction, action, parsetree->mergeActionList)
		{
			if (action->commandType != CMD_NOTHING &&
				view_has_instead_trigger(view, action->commandType, NIL))
				ereport(ERROR,
						errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot merge into view \"%s\"",
							   RelationGetRelationName(view)),
						errdetail("MERGE is not supported for views with INSTEAD OF triggers for some actions but not all."),
						errhint("To enable merging into the view, either provide a full set of INSTEAD OF triggers or drop the existing INSTEAD OF triggers."));
		}
	}

	/*
	 * If we get here, view_query_is_auto_updatable() has verified that the
	 * view contains a single base relation.
	 */
	Assert(list_length(viewquery->jointree->fromlist) == 1);
	rtr = linitial_node(RangeTblRef, viewquery->jointree->fromlist);

	base_rt_index = rtr->rtindex;
	base_rte = rt_fetch(base_rt_index, viewquery->rtable);
	Assert(base_rte->rtekind == RTE_RELATION);
	base_perminfo = getRTEPermissionInfo(viewquery->rteperminfos, base_rte);

	/*
	 * Up to now, the base relation hasn't been touched at all in our query.
	 * We need to acquire lock on it before we try to do anything with it.
	 * (The subsequent recursive call of RewriteQuery will suppose that we
	 * already have the right lock!)  Since it will become the query target
	 * relation, RowExclusiveLock is always the right thing.
	 */
	base_rel = table_open(base_rte->relid, RowExclusiveLock);

	/*
	 * While we have the relation open, update the RTE's relkind, just in case
	 * it changed since this view was made (cf. AcquireRewriteLocks).
	 */
	base_rte->relkind = base_rel->rd_rel->relkind;

	/*
	 * If the view query contains any sublink subqueries then we need to also
	 * acquire locks on any relations they refer to.  We know that there won't
	 * be any subqueries in the range table or CTEs, so we can skip those, as
	 * in AcquireRewriteLocks.
	 */
	if (viewquery->hasSubLinks)
	{
		acquireLocksOnSubLinks_context context;

		context.for_execute = true;
		query_tree_walker(viewquery, acquireLocksOnSubLinks, &context,
						  QTW_IGNORE_RC_SUBQUERIES);
	}

	/*
	 * Create a new target RTE describing the base relation, and add it to the
	 * outer query's rangetable.  (What's happening in the next few steps is
	 * very much like what the planner would do to "pull up" the view into the
	 * outer query.  Perhaps someday we should refactor things enough so that
	 * we can share code with the planner.)
	 *
	 * Be sure to set rellockmode to the correct thing for the target table.
	 * Since we copied the whole viewquery above, we can just scribble on
	 * base_rte instead of copying it.
	 */
	new_rte = base_rte;
	new_rte->rellockmode = RowExclusiveLock;

	parsetree->rtable = lappend(parsetree->rtable, new_rte);
	new_rt_index = list_length(parsetree->rtable);

	/*
	 * INSERTs never inherit.  For UPDATE/DELETE/MERGE, we use the view
	 * query's inheritance flag for the base relation.
	 */
	if (parsetree->commandType == CMD_INSERT)
		new_rte->inh = false;

	/*
	 * Adjust the view's targetlist Vars to reference the new target RTE, ie
	 * make their varnos be new_rt_index instead of base_rt_index.  There can
	 * be no Vars for other rels in the tlist, so this is sufficient to pull
	 * up the tlist expressions for use in the outer query.  The tlist will
	 * provide the replacement expressions used by ReplaceVarsFromTargetList
	 * below.
	 */
	view_targetlist = viewquery->targetList;

	ChangeVarNodes((Node *) view_targetlist,
				   base_rt_index,
				   new_rt_index,
				   0);

	/*
	 * If the view has "security_invoker" set, mark the new target relation
	 * for the permissions checks that we want to enforce against the query
	 * caller. Otherwise we want to enforce them against the view owner.
	 *
	 * At the relation level, require the same INSERT/UPDATE/DELETE
	 * permissions that the query caller needs against the view.  We drop the
	 * ACL_SELECT bit that is presumably in new_perminfo->requiredPerms
	 * initially.
	 *
	 * Note: the original view's RTEPermissionInfo remains in the query's
	 * rteperminfos so that the executor still performs appropriate
	 * permissions checks for the query caller's use of the view.
	 *
	 * Disregard the perminfo in viewquery->rteperminfos that the base_rte
	 * would currently be pointing at, because we'd like it to point now to a
	 * new one that will be filled below.  Must set perminfoindex to 0 to not
	 * trip over the Assert in addRTEPermissionInfo().
	 */
	new_rte->perminfoindex = 0;
	new_perminfo = addRTEPermissionInfo(&parsetree->rteperminfos, new_rte);
	if (RelationHasSecurityInvoker(view))
		new_perminfo->checkAsUser = InvalidOid;
	else
		new_perminfo->checkAsUser = view->rd_rel->relowner;
	new_perminfo->requiredPerms = view_perminfo->requiredPerms;

	/*
	 * Now for the per-column permissions bits.
	 *
	 * Initially, new_perminfo (base_perminfo) contains selectedCols
	 * permission check bits for all base-rel columns referenced by the view,
	 * but since the view is a SELECT query its insertedCols/updatedCols is
	 * empty.  We set insertedCols and updatedCols to include all the columns
	 * the outer query is trying to modify, adjusting the column numbers as
	 * needed.  But we leave selectedCols as-is, so the view owner must have
	 * read permission for all columns used in the view definition, even if
	 * some of them are not read by the outer query.  We could try to limit
	 * selectedCols to only columns used in the transformed query, but that
	 * does not correspond to what happens in ordinary SELECT usage of a view:
	 * all referenced columns must have read permission, even if optimization
	 * finds that some of them can be discarded during query transformation.
	 * The flattening we're doing here is an optional optimization, too.  (If
	 * you are unpersuaded and want to change this, note that applying
	 * adjust_view_column_set to view_perminfo->selectedCols is clearly *not*
	 * the right answer, since that neglects base-rel columns used in the
	 * view's WHERE quals.)
	 *
	 * This step needs the modified view targetlist, so we have to do things
	 * in this order.
	 */
	Assert(bms_is_empty(new_perminfo->insertedCols) &&
		   bms_is_empty(new_perminfo->updatedCols));

	new_perminfo->selectedCols = base_perminfo->selectedCols;

	new_perminfo->insertedCols =
		adjust_view_column_set(view_perminfo->insertedCols, view_targetlist);

	new_perminfo->updatedCols =
		adjust_view_column_set(view_perminfo->updatedCols, view_targetlist);

	/*
	 * Move any security barrier quals from the view RTE onto the new target
	 * RTE.  Any such quals should now apply to the new target RTE and will
	 * not reference the original view RTE in the rewritten query.
	 */
	new_rte->securityQuals = view_rte->securityQuals;
	view_rte->securityQuals = NIL;

	/*
	 * Now update all Vars in the outer query that reference the view to
	 * reference the appropriate column of the base relation instead.
	 */
	parsetree = (Query *)
		ReplaceVarsFromTargetList((Node *) parsetree,
								  parsetree->resultRelation,
								  0,
								  view_rte,
								  view_targetlist,
								  new_rt_index,
								  REPLACEVARS_REPORT_ERROR,
								  0,
								  NULL);

	/*
	 * Update all other RTI references in the query that point to the view
	 * (for example, parsetree->resultRelation itself) to point to the new
	 * base relation instead.  Vars will not be affected since none of them
	 * reference parsetree->resultRelation any longer.
	 */
	ChangeVarNodes((Node *) parsetree,
				   parsetree->resultRelation,
				   new_rt_index,
				   0);
	Assert(parsetree->resultRelation == new_rt_index);

	/*
	 * For INSERT/UPDATE we must also update resnos in the targetlist to refer
	 * to columns of the base relation, since those indicate the target
	 * columns to be affected.  Similarly, for MERGE we must update the resnos
	 * in the merge action targetlists of any INSERT/UPDATE actions.
	 *
	 * Note that this destroys the resno ordering of the targetlists, but that
	 * will be fixed when we recurse through RewriteQuery, which will invoke
	 * rewriteTargetListIU again on the updated targetlists.
	 */
	if (parsetree->commandType != CMD_DELETE)
	{
		foreach(lc, parsetree->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			TargetEntry *view_tle;

			if (tle->resjunk)
				continue;

			view_tle = get_tle_by_resno(view_targetlist, tle->resno);
			if (view_tle != NULL && !view_tle->resjunk && IsA(view_tle->expr, Var))
				tle->resno = ((Var *) view_tle->expr)->varattno;
			else
				elog(ERROR, "attribute number %d not found in view targetlist",
					 tle->resno);
		}

		foreach_node(MergeAction, action, parsetree->mergeActionList)
		{
			if (action->commandType == CMD_INSERT ||
				action->commandType == CMD_UPDATE)
			{
				foreach_node(TargetEntry, tle, action->targetList)
				{
					TargetEntry *view_tle;

					if (tle->resjunk)
						continue;

					view_tle = get_tle_by_resno(view_targetlist, tle->resno);
					if (view_tle != NULL && !view_tle->resjunk && IsA(view_tle->expr, Var))
						tle->resno = ((Var *) view_tle->expr)->varattno;
					else
						elog(ERROR, "attribute number %d not found in view targetlist",
							 tle->resno);
				}
			}
		}
	}

	/*
	 * For INSERT .. ON CONFLICT .. DO UPDATE, we must also update assorted
	 * stuff in the onConflict data structure.
	 */
	if (parsetree->onConflict &&
		parsetree->onConflict->action == ONCONFLICT_UPDATE)
	{
		Index		old_exclRelIndex,
					new_exclRelIndex;
		ParseNamespaceItem *new_exclNSItem;
		RangeTblEntry *new_exclRte;
		List	   *tmp_tlist;

		/*
		 * Like the INSERT/UPDATE code above, update the resnos in the
		 * auxiliary UPDATE targetlist to refer to columns of the base
		 * relation.
		 */
		foreach(lc, parsetree->onConflict->onConflictSet)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			TargetEntry *view_tle;

			if (tle->resjunk)
				continue;

			view_tle = get_tle_by_resno(view_targetlist, tle->resno);
			if (view_tle != NULL && !view_tle->resjunk && IsA(view_tle->expr, Var))
				tle->resno = ((Var *) view_tle->expr)->varattno;
			else
				elog(ERROR, "attribute number %d not found in view targetlist",
					 tle->resno);
		}

		/*
		 * Also, create a new RTE for the EXCLUDED pseudo-relation, using the
		 * query's new base rel (which may well have a different column list
		 * from the view, hence we need a new column alias list).  This should
		 * match transformOnConflictClause.  In particular, note that the
		 * relkind is set to composite to signal that we're not dealing with
		 * an actual relation.
		 */
		old_exclRelIndex = parsetree->onConflict->exclRelIndex;

		new_exclNSItem = addRangeTableEntryForRelation(make_parsestate(NULL),
													   base_rel,
													   RowExclusiveLock,
													   makeAlias("excluded", NIL),
													   false, false);
		new_exclRte = new_exclNSItem->p_rte;
		new_exclRte->relkind = RELKIND_COMPOSITE_TYPE;
		/* Ignore the RTEPermissionInfo that would've been added. */
		new_exclRte->perminfoindex = 0;

		parsetree->rtable = lappend(parsetree->rtable, new_exclRte);
		new_exclRelIndex = parsetree->onConflict->exclRelIndex =
			list_length(parsetree->rtable);

		/*
		 * Replace the targetlist for the EXCLUDED pseudo-relation with a new
		 * one, representing the columns from the new base relation.
		 */
		parsetree->onConflict->exclRelTlist =
			BuildOnConflictExcludedTargetlist(base_rel, new_exclRelIndex);

		/*
		 * Update all Vars in the ON CONFLICT clause that refer to the old
		 * EXCLUDED pseudo-relation.  We want to use the column mappings
		 * defined in the view targetlist, but we need the outputs to refer to
		 * the new EXCLUDED pseudo-relation rather than the new target RTE.
		 * Also notice that "EXCLUDED.*" will be expanded using the view's
		 * rowtype, which seems correct.
		 */
		tmp_tlist = copyObject(view_targetlist);

		ChangeVarNodes((Node *) tmp_tlist, new_rt_index,
					   new_exclRelIndex, 0);

		parsetree->onConflict = (OnConflictExpr *)
			ReplaceVarsFromTargetList((Node *) parsetree->onConflict,
									  old_exclRelIndex,
									  0,
									  view_rte,
									  tmp_tlist,
									  new_rt_index,
									  REPLACEVARS_REPORT_ERROR,
									  0,
									  &parsetree->hasSubLinks);
	}

	/*
	 * For UPDATE/DELETE/MERGE, pull up any WHERE quals from the view.  We
	 * know that any Vars in the quals must reference the one base relation,
	 * so we need only adjust their varnos to reference the new target (just
	 * the same as we did with the view targetlist).
	 *
	 * If it's a security-barrier view, its WHERE quals must be applied before
	 * quals from the outer query, so we attach them to the RTE as security
	 * barrier quals rather than adding them to the main WHERE clause.
	 *
	 * For INSERT, the view's quals can be ignored in the main query.
	 */
	if (parsetree->commandType != CMD_INSERT &&
		viewquery->jointree->quals != NULL)
	{
		Node	   *viewqual = (Node *) viewquery->jointree->quals;

		/*
		 * Even though we copied viewquery already at the top of this
		 * function, we must duplicate the viewqual again here, because we may
		 * need to use the quals again below for a WithCheckOption clause.
		 */
		viewqual = copyObject(viewqual);

		ChangeVarNodes(viewqual, base_rt_index, new_rt_index, 0);

		if (RelationIsSecurityView(view))
		{
			/*
			 * The view's quals go in front of existing barrier quals: those
			 * would have come from an outer level of security-barrier view,
			 * and so must get evaluated later.
			 *
			 * Note: the parsetree has been mutated, so the new_rte pointer is
			 * stale and needs to be re-computed.
			 */
			new_rte = rt_fetch(new_rt_index, parsetree->rtable);
			new_rte->securityQuals = lcons(viewqual, new_rte->securityQuals);

			/*
			 * Do not set parsetree->hasRowSecurity, because these aren't RLS
			 * conditions (they aren't affected by enabling/disabling RLS).
			 */

			/*
			 * Make sure that the query is marked correctly if the added qual
			 * has sublinks.
			 */
			if (!parsetree->hasSubLinks)
				parsetree->hasSubLinks = checkExprHasSubLink(viewqual);
		}
		else
			AddQual(parsetree, (Node *) viewqual);
	}

	/*
	 * For INSERT/UPDATE (or MERGE containing INSERT/UPDATE), if the view has
	 * the WITH CHECK OPTION, or any parent view specified WITH CASCADED CHECK
	 * OPTION, add the quals from the view to the query's withCheckOptions
	 * list.
	 */
	if (insert_or_update)
	{
		bool		has_wco = RelationHasCheckOption(view);
		bool		cascaded = RelationHasCascadedCheckOption(view);

		/*
		 * If the parent view has a cascaded check option, treat this view as
		 * if it also had a cascaded check option.
		 *
		 * New WithCheckOptions are added to the start of the list, so if
		 * there is a cascaded check option, it will be the first item in the
		 * list.
		 */
		if (parsetree->withCheckOptions != NIL)
		{
			WithCheckOption *parent_wco =
				(WithCheckOption *) linitial(parsetree->withCheckOptions);

			if (parent_wco->cascaded)
			{
				has_wco = true;
				cascaded = true;
			}
		}

		/*
		 * Add the new WithCheckOption to the start of the list, so that
		 * checks on inner views are run before checks on outer views, as
		 * required by the SQL standard.
		 *
		 * If the new check is CASCADED, we need to add it even if this view
		 * has no quals, since there may be quals on child views.  A LOCAL
		 * check can be omitted if this view has no quals.
		 */
		if (has_wco && (cascaded || viewquery->jointree->quals != NULL))
		{
			WithCheckOption *wco;

			wco = makeNode(WithCheckOption);
			wco->kind = WCO_VIEW_CHECK;
			wco->relname = pstrdup(RelationGetRelationName(view));
			wco->polname = NULL;
			wco->qual = NULL;
			wco->cascaded = cascaded;

			parsetree->withCheckOptions = lcons(wco,
												parsetree->withCheckOptions);

			if (viewquery->jointree->quals != NULL)
			{
				wco->qual = (Node *) viewquery->jointree->quals;
				ChangeVarNodes(wco->qual, base_rt_index, new_rt_index, 0);

				/*
				 * For INSERT, make sure that the query is marked correctly if
				 * the added qual has sublinks.  This can be skipped for
				 * UPDATE/MERGE, since the same qual will have already been
				 * added above, and the check will already have been done.
				 */
				if (!parsetree->hasSubLinks &&
					parsetree->commandType == CMD_INSERT)
					parsetree->hasSubLinks = checkExprHasSubLink(wco->qual);
			}
		}
	}

	table_close(base_rel, NoLock);

	return parsetree;
}


/*
 * RewriteQuery -
 *	  rewrites the query and apply the rules again on the queries rewritten
 *
 * rewrite_events is a list of open query-rewrite actions, so we can detect
 * infinite recursion.
 *
 * orig_rt_length is the length of the originating query's rtable, for product
 * queries created by fireRules(), and 0 otherwise.  This is used to skip any
 * already-processed VALUES RTEs from the original query.
 */
static List *
RewriteQuery(Query *parsetree, List *rewrite_events, int orig_rt_length)
{
	CmdType		event = parsetree->commandType;
	bool		instead = false;
	bool		returning = false;
	bool		updatableview = false;
	Query	   *qual_product = NULL;
	List	   *rewritten = NIL;
	ListCell   *lc1;

	/*
	 * First, recursively process any insert/update/delete/merge statements in
	 * WITH clauses.  (We have to do this first because the WITH clauses may
	 * get copied into rule actions below.)
	 */
	foreach(lc1, parsetree->cteList)
	{
		CommonTableExpr *cte = lfirst_node(CommonTableExpr, lc1);
		Query	   *ctequery = castNode(Query, cte->ctequery);
		List	   *newstuff;

		if (ctequery->commandType == CMD_SELECT)
			continue;

		newstuff = RewriteQuery(ctequery, rewrite_events, 0);

		/*
		 * Currently we can only handle unconditional, single-statement DO
		 * INSTEAD rules correctly; we have to get exactly one non-utility
		 * Query out of the rewrite operation to stuff back into the CTE node.
		 */
		if (list_length(newstuff) == 1)
		{
			/* Must check it's not a utility command */
			ctequery = linitial_node(Query, newstuff);
			if (!(ctequery->commandType == CMD_SELECT ||
				  ctequery->commandType == CMD_UPDATE ||
				  ctequery->commandType == CMD_INSERT ||
				  ctequery->commandType == CMD_DELETE ||
				  ctequery->commandType == CMD_MERGE))
			{
				/*
				 * Currently it could only be NOTIFY; this error message will
				 * need work if we ever allow other utility commands in rules.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("DO INSTEAD NOTIFY rules are not supported for data-modifying statements in WITH")));
			}
			/* WITH queries should never be canSetTag */
			Assert(!ctequery->canSetTag);
			/* Push the single Query back into the CTE node */
			cte->ctequery = (Node *) ctequery;
		}
		else if (newstuff == NIL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("DO INSTEAD NOTHING rules are not supported for data-modifying statements in WITH")));
		}
		else
		{
			ListCell   *lc2;

			/* examine queries to determine which error message to issue */
			foreach(lc2, newstuff)
			{
				Query	   *q = (Query *) lfirst(lc2);

				if (q->querySource == QSRC_QUAL_INSTEAD_RULE)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("conditional DO INSTEAD rules are not supported for data-modifying statements in WITH")));
				if (q->querySource == QSRC_NON_INSTEAD_RULE)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("DO ALSO rules are not supported for data-modifying statements in WITH")));
			}

			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("multi-statement DO INSTEAD rules are not supported for data-modifying statements in WITH")));
		}
	}

	/*
	 * If the statement is an insert, update, delete, or merge, adjust its
	 * targetlist as needed, and then fire INSERT/UPDATE/DELETE rules on it.
	 *
	 * SELECT rules are handled later when we have all the queries that should
	 * get executed.  Also, utilities aren't rewritten at all (do we still
	 * need that check?)
	 */
	if (event != CMD_SELECT && event != CMD_UTILITY)
	{
		int			result_relation;
		RangeTblEntry *rt_entry;
		Relation	rt_entry_relation;
		List	   *locks;
		int			product_orig_rt_length;
		List	   *product_queries;
		bool		hasUpdate = false;
		int			values_rte_index = 0;
		bool		defaults_remaining = false;

		result_relation = parsetree->resultRelation;
		Assert(result_relation != 0);
		rt_entry = rt_fetch(result_relation, parsetree->rtable);
		Assert(rt_entry->rtekind == RTE_RELATION);

		/*
		 * We can use NoLock here since either the parser or
		 * AcquireRewriteLocks should have locked the rel already.
		 */
		rt_entry_relation = table_open(rt_entry->relid, NoLock);

		/*
		 * Rewrite the targetlist as needed for the command type.
		 */
		if (event == CMD_INSERT)
		{
			ListCell   *lc2;
			RangeTblEntry *values_rte = NULL;

			/*
			 * Test if it's a multi-row INSERT ... VALUES (...), (...), ... by
			 * looking for a VALUES RTE in the fromlist.  For product queries,
			 * we must ignore any already-processed VALUES RTEs from the
			 * original query.  These appear at the start of the rangetable.
			 */
			foreach(lc2, parsetree->jointree->fromlist)
			{
				RangeTblRef *rtr = (RangeTblRef *) lfirst(lc2);

				if (IsA(rtr, RangeTblRef) && rtr->rtindex > orig_rt_length)
				{
					RangeTblEntry *rte = rt_fetch(rtr->rtindex,
												  parsetree->rtable);

					if (rte->rtekind == RTE_VALUES)
					{
						/* should not find more than one VALUES RTE */
						if (values_rte != NULL)
							elog(ERROR, "more than one VALUES RTE found");

						values_rte = rte;
						values_rte_index = rtr->rtindex;
					}
				}
			}

			if (values_rte)
			{
				Bitmapset  *unused_values_attrnos = NULL;

				/* Process the main targetlist ... */
				parsetree->targetList = rewriteTargetListIU(parsetree->targetList,
															parsetree->commandType,
															parsetree->override,
															rt_entry_relation,
															values_rte,
															values_rte_index,
															&unused_values_attrnos);
				/* ... and the VALUES expression lists */
				if (!rewriteValuesRTE(parsetree, values_rte, values_rte_index,
									  rt_entry_relation,
									  unused_values_attrnos))
					defaults_remaining = true;
			}
			else
			{
				/* Process just the main targetlist */
				parsetree->targetList =
					rewriteTargetListIU(parsetree->targetList,
										parsetree->commandType,
										parsetree->override,
										rt_entry_relation,
										NULL, 0, NULL);
			}

			if (parsetree->onConflict &&
				parsetree->onConflict->action == ONCONFLICT_UPDATE)
			{
				parsetree->onConflict->onConflictSet =
					rewriteTargetListIU(parsetree->onConflict->onConflictSet,
										CMD_UPDATE,
										parsetree->override,
										rt_entry_relation,
										NULL, 0, NULL);
			}
		}
		else if (event == CMD_UPDATE)
		{
			Assert(parsetree->override == OVERRIDING_NOT_SET);
			parsetree->targetList =
				rewriteTargetListIU(parsetree->targetList,
									parsetree->commandType,
									parsetree->override,
									rt_entry_relation,
									NULL, 0, NULL);
		}
		else if (event == CMD_MERGE)
		{
			Assert(parsetree->override == OVERRIDING_NOT_SET);

			/*
			 * Rewrite each action targetlist separately
			 */
			foreach(lc1, parsetree->mergeActionList)
			{
				MergeAction *action = (MergeAction *) lfirst(lc1);

				switch (action->commandType)
				{
					case CMD_NOTHING:
					case CMD_DELETE:	/* Nothing to do here */
						break;
					case CMD_UPDATE:
					case CMD_INSERT:

						/*
						 * MERGE actions do not permit multi-row INSERTs, so
						 * there is no VALUES RTE to deal with here.
						 */
						action->targetList =
							rewriteTargetListIU(action->targetList,
												action->commandType,
												action->override,
												rt_entry_relation,
												NULL, 0, NULL);
						break;
					default:
						elog(ERROR, "unrecognized commandType: %d", action->commandType);
						break;
				}
			}
		}
		else if (event == CMD_DELETE)
		{
			/* Nothing to do here */
		}
		else
			elog(ERROR, "unrecognized commandType: %d", (int) event);

		/*
		 * Collect and apply the appropriate rules.
		 */
		locks = matchLocks(event, rt_entry_relation,
						   result_relation, parsetree, &hasUpdate);

		product_orig_rt_length = list_length(parsetree->rtable);
		product_queries = fireRules(parsetree,
									result_relation,
									event,
									locks,
									&instead,
									&returning,
									&qual_product);

		/*
		 * If we have a VALUES RTE with any remaining untouched DEFAULT items,
		 * and we got any product queries, finalize the VALUES RTE for each
		 * product query (replacing the remaining DEFAULT items with NULLs).
		 * We don't do this for the original query, because we know that it
		 * must be an auto-insert on a view, and so should use the base
		 * relation's defaults for any remaining DEFAULT items.
		 */
		if (defaults_remaining && product_queries != NIL)
		{
			ListCell   *n;

			/*
			 * Each product query has its own copy of the VALUES RTE at the
			 * same index in the rangetable, so we must finalize each one.
			 *
			 * Note that if the product query is an INSERT ... SELECT, then
			 * the VALUES RTE will be at the same index in the SELECT part of
			 * the product query rather than the top-level product query
			 * itself.
			 */
			foreach(n, product_queries)
			{
				Query	   *pt = (Query *) lfirst(n);
				RangeTblEntry *values_rte;

				if (pt->commandType == CMD_INSERT &&
					pt->jointree && IsA(pt->jointree, FromExpr) &&
					list_length(pt->jointree->fromlist) == 1)
				{
					Node	   *jtnode = (Node *) linitial(pt->jointree->fromlist);

					if (IsA(jtnode, RangeTblRef))
					{
						int			rtindex = ((RangeTblRef *) jtnode)->rtindex;
						RangeTblEntry *src_rte = rt_fetch(rtindex, pt->rtable);

						if (src_rte->rtekind == RTE_SUBQUERY &&
							src_rte->subquery &&
							IsA(src_rte->subquery, Query) &&
							src_rte->subquery->commandType == CMD_SELECT)
							pt = src_rte->subquery;
					}
				}

				values_rte = rt_fetch(values_rte_index, pt->rtable);
				if (values_rte->rtekind != RTE_VALUES)
					elog(ERROR, "failed to find VALUES RTE in product query");

				rewriteValuesRTEToNulls(pt, values_rte);
			}
		}

		/*
		 * If there was no unqualified INSTEAD rule, and the target relation
		 * is a view without any INSTEAD OF triggers, see if the view can be
		 * automatically updated.  If so, we perform the necessary query
		 * transformation here and add the resulting query to the
		 * product_queries list, so that it gets recursively rewritten if
		 * necessary.  For MERGE, the view must be automatically updatable if
		 * any of the merge actions lack a corresponding INSTEAD OF trigger.
		 *
		 * If the view cannot be automatically updated, we throw an error here
		 * which is OK since the query would fail at runtime anyway.  Throwing
		 * the error here is preferable to the executor check since we have
		 * more detailed information available about why the view isn't
		 * updatable.
		 */
		if (!instead &&
			rt_entry_relation->rd_rel->relkind == RELKIND_VIEW &&
			!view_has_instead_trigger(rt_entry_relation, event,
									  parsetree->mergeActionList))
		{
			/*
			 * If there were any qualified INSTEAD rules, don't allow the view
			 * to be automatically updated (an unqualified INSTEAD rule or
			 * INSTEAD OF trigger is required).
			 */
			if (qual_product != NULL)
				error_view_not_updatable(rt_entry_relation,
										 parsetree->commandType,
										 parsetree->mergeActionList,
										 gettext_noop("Views with conditional DO INSTEAD rules are not automatically updatable."));

			/*
			 * Attempt to rewrite the query to automatically update the view.
			 * This throws an error if the view can't be automatically
			 * updated.
			 */
			parsetree = rewriteTargetView(parsetree, rt_entry_relation);

			/*
			 * At this point product_queries contains any DO ALSO rule
			 * actions. Add the rewritten query before or after those.  This
			 * must match the handling the original query would have gotten
			 * below, if we allowed it to be included again.
			 */
			if (parsetree->commandType == CMD_INSERT)
				product_queries = lcons(parsetree, product_queries);
			else
				product_queries = lappend(product_queries, parsetree);

			/*
			 * Set the "instead" flag, as if there had been an unqualified
			 * INSTEAD, to prevent the original query from being included a
			 * second time below.  The transformation will have rewritten any
			 * RETURNING list, so we can also set "returning" to forestall
			 * throwing an error below.
			 */
			instead = true;
			returning = true;
			updatableview = true;
		}

		/*
		 * If we got any product queries, recursively rewrite them --- but
		 * first check for recursion!
		 */
		if (product_queries != NIL)
		{
			ListCell   *n;
			rewrite_event *rev;

			foreach(n, rewrite_events)
			{
				rev = (rewrite_event *) lfirst(n);
				if (rev->relation == RelationGetRelid(rt_entry_relation) &&
					rev->event == event)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("infinite recursion detected in rules for relation \"%s\"",
									RelationGetRelationName(rt_entry_relation))));
			}

			rev = (rewrite_event *) palloc(sizeof(rewrite_event));
			rev->relation = RelationGetRelid(rt_entry_relation);
			rev->event = event;
			rewrite_events = lappend(rewrite_events, rev);

			foreach(n, product_queries)
			{
				Query	   *pt = (Query *) lfirst(n);
				List	   *newstuff;

				/*
				 * For an updatable view, pt might be the rewritten version of
				 * the original query, in which case we pass on orig_rt_length
				 * to finish processing any VALUES RTE it contained.
				 *
				 * Otherwise, we have a product query created by fireRules().
				 * Any VALUES RTEs from the original query have been fully
				 * processed, and must be skipped when we recurse.
				 */
				newstuff = RewriteQuery(pt, rewrite_events,
										pt == parsetree ?
										orig_rt_length :
										product_orig_rt_length);
				rewritten = list_concat(rewritten, newstuff);
			}

			rewrite_events = list_delete_last(rewrite_events);
		}

		/*
		 * If there is an INSTEAD, and the original query has a RETURNING, we
		 * have to have found a RETURNING in the rule(s), else fail. (Because
		 * DefineQueryRewrite only allows RETURNING in unconditional INSTEAD
		 * rules, there's no need to worry whether the substituted RETURNING
		 * will actually be executed --- it must be.)
		 */
		if ((instead || qual_product != NULL) &&
			parsetree->returningList &&
			!returning)
		{
			switch (event)
			{
				case CMD_INSERT:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot perform INSERT RETURNING on relation \"%s\"",
									RelationGetRelationName(rt_entry_relation)),
							 errhint("You need an unconditional ON INSERT DO INSTEAD rule with a RETURNING clause.")));
					break;
				case CMD_UPDATE:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot perform UPDATE RETURNING on relation \"%s\"",
									RelationGetRelationName(rt_entry_relation)),
							 errhint("You need an unconditional ON UPDATE DO INSTEAD rule with a RETURNING clause.")));
					break;
				case CMD_DELETE:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot perform DELETE RETURNING on relation \"%s\"",
									RelationGetRelationName(rt_entry_relation)),
							 errhint("You need an unconditional ON DELETE DO INSTEAD rule with a RETURNING clause.")));
					break;
				default:
					elog(ERROR, "unrecognized commandType: %d",
						 (int) event);
					break;
			}
		}

		/*
		 * Updatable views are supported by ON CONFLICT, so don't prevent that
		 * case from proceeding
		 */
		if (parsetree->onConflict &&
			(product_queries != NIL || hasUpdate) &&
			!updatableview)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INSERT with ON CONFLICT clause cannot be used with table that has INSERT or UPDATE rules")));

		table_close(rt_entry_relation, NoLock);
	}

	/*
	 * For INSERTs, the original query is done first; for UPDATE/DELETE, it is
	 * done last.  This is needed because update and delete rule actions might
	 * not do anything if they are invoked after the update or delete is
	 * performed. The command counter increment between the query executions
	 * makes the deleted (and maybe the updated) tuples disappear so the scans
	 * for them in the rule actions cannot find them.
	 *
	 * If we found any unqualified INSTEAD, the original query is not done at
	 * all, in any form.  Otherwise, we add the modified form if qualified
	 * INSTEADs were found, else the unmodified form.
	 */
	if (!instead)
	{
		if (parsetree->commandType == CMD_INSERT)
		{
			if (qual_product != NULL)
				rewritten = lcons(qual_product, rewritten);
			else
				rewritten = lcons(parsetree, rewritten);
		}
		else
		{
			if (qual_product != NULL)
				rewritten = lappend(rewritten, qual_product);
			else
				rewritten = lappend(rewritten, parsetree);
		}
	}

	/*
	 * If the original query has a CTE list, and we generated more than one
	 * non-utility result query, we have to fail because we'll have copied the
	 * CTE list into each result query.  That would break the expectation of
	 * single evaluation of CTEs.  This could possibly be fixed by
	 * restructuring so that a CTE list can be shared across multiple Query
	 * and PlannableStatement nodes.
	 */
	if (parsetree->cteList != NIL)
	{
		int			qcount = 0;

		foreach(lc1, rewritten)
		{
			Query	   *q = (Query *) lfirst(lc1);

			if (q->commandType != CMD_UTILITY)
				qcount++;
		}
		if (qcount > 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("WITH cannot be used in a query that is rewritten by rules into multiple queries")));
	}

	return rewritten;
}


/*
 * Expand virtual generated columns
 *
 * If the table contains virtual generated columns, build a target list
 * containing the expanded expressions and use ReplaceVarsFromTargetList() to
 * do the replacements.
 *
 * Vars matching rt_index at the current query level are replaced by the
 * virtual generated column expressions from rel, if there are any.
 *
 * The caller must also provide rte, the RTE describing the target relation,
 * in order to handle any whole-row Vars referencing the target, and
 * result_relation, the index of the result relation, if this is part of an
 * INSERT/UPDATE/DELETE/MERGE query.
 */
static Node *
expand_generated_columns_internal(Node *node, Relation rel, int rt_index,
								  RangeTblEntry *rte, int result_relation)
{
	TupleDesc	tupdesc;

	tupdesc = RelationGetDescr(rel);
	if (tupdesc->constr && tupdesc->constr->has_generated_virtual)
	{
		List	   *tlist = NIL;

		for (int i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

			if (attr->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
			{
				Node	   *defexpr;
				TargetEntry *te;

				defexpr = build_generation_expression(rel, i + 1);
				ChangeVarNodes(defexpr, 1, rt_index, 0);

				te = makeTargetEntry((Expr *) defexpr, i + 1, 0, false);
				tlist = lappend(tlist, te);
			}
		}

		Assert(list_length(tlist) > 0);

		node = ReplaceVarsFromTargetList(node, rt_index, 0, rte, tlist,
										 result_relation,
										 REPLACEVARS_CHANGE_VARNO, rt_index,
										 NULL);
	}

	return node;
}

/*
 * Expand virtual generated columns in an expression
 *
 * This is for expressions that are not part of a query, such as default
 * expressions or index predicates.  The rt_index is usually 1.
 */
Node *
expand_generated_columns_in_expr(Node *node, Relation rel, int rt_index)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);

	if (tupdesc->constr && tupdesc->constr->has_generated_virtual)
	{
		RangeTblEntry *rte;

		rte = makeNode(RangeTblEntry);
		/* eref needs to be set, but the actual name doesn't matter */
		rte->eref = makeAlias(RelationGetRelationName(rel), NIL);
		rte->rtekind = RTE_RELATION;
		rte->relid = RelationGetRelid(rel);

		node = expand_generated_columns_internal(node, rel, rt_index, rte, 0);
	}

	return node;
}

/*
 * Build the generation expression for the virtual generated column.
 *
 * Error out if there is no generation expression found for the given column.
 */
Node *
build_generation_expression(Relation rel, int attrno)
{
	TupleDesc	rd_att = RelationGetDescr(rel);
	Form_pg_attribute att_tup = TupleDescAttr(rd_att, attrno - 1);
	Node	   *defexpr;
	Oid			attcollid;

	Assert(rd_att->constr && rd_att->constr->has_generated_virtual);
	Assert(att_tup->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL);

	defexpr = build_column_default(rel, attrno);
	if (defexpr == NULL)
		elog(ERROR, "no generation expression found for column number %d of table \"%s\"",
			 attrno, RelationGetRelationName(rel));

	/*
	 * If the column definition has a collation and it is different from the
	 * collation of the generation expression, put a COLLATE clause around the
	 * expression.
	 */
	attcollid = att_tup->attcollation;
	if (attcollid && attcollid != exprCollation(defexpr))
	{
		CollateExpr *ce = makeNode(CollateExpr);

		ce->arg = (Expr *) defexpr;
		ce->collOid = attcollid;
		ce->location = -1;

		defexpr = (Node *) ce;
	}

	return defexpr;
}


/*
 * QueryRewrite -
 *	  Primary entry point to the query rewriter.
 *	  Rewrite one query via query rewrite system, possibly returning 0
 *	  or many queries.
 *
 * NOTE: the parsetree must either have come straight from the parser,
 * or have been scanned by AcquireRewriteLocks to acquire suitable locks.
 */
List *
QueryRewrite(Query *parsetree)
{
	int64		input_query_id = parsetree->queryId;
	List	   *querylist;
	List	   *results;
	ListCell   *l;
	CmdType		origCmdType;
	bool		foundOriginalQuery;
	Query	   *lastInstead;

	/*
	 * This function is only applied to top-level original queries
	 */
	Assert(parsetree->querySource == QSRC_ORIGINAL);
	Assert(parsetree->canSetTag);

	/*
	 * Step 1
	 *
	 * Apply all non-SELECT rules possibly getting 0 or many queries
	 */
	querylist = RewriteQuery(parsetree, NIL, 0);

	/*
	 * Step 2
	 *
	 * Apply all the RIR rules on each query
	 *
	 * This is also a handy place to mark each query with the original queryId
	 */
	results = NIL;
	foreach(l, querylist)
	{
		Query	   *query = (Query *) lfirst(l);

		query = fireRIRrules(query, NIL);

		query->queryId = input_query_id;

		results = lappend(results, query);
	}

	/*
	 * Step 3
	 *
	 * Determine which, if any, of the resulting queries is supposed to set
	 * the command-result tag; and update the canSetTag fields accordingly.
	 *
	 * If the original query is still in the list, it sets the command tag.
	 * Otherwise, the last INSTEAD query of the same kind as the original is
	 * allowed to set the tag.  (Note these rules can leave us with no query
	 * setting the tag.  The tcop code has to cope with this by setting up a
	 * default tag based on the original un-rewritten query.)
	 *
	 * The Asserts verify that at most one query in the result list is marked
	 * canSetTag.  If we aren't checking asserts, we can fall out of the loop
	 * as soon as we find the original query.
	 */
	origCmdType = parsetree->commandType;
	foundOriginalQuery = false;
	lastInstead = NULL;

	foreach(l, results)
	{
		Query	   *query = (Query *) lfirst(l);

		if (query->querySource == QSRC_ORIGINAL)
		{
			Assert(query->canSetTag);
			Assert(!foundOriginalQuery);
			foundOriginalQuery = true;
#ifndef USE_ASSERT_CHECKING
			break;
#endif
		}
		else
		{
			Assert(!query->canSetTag);
			if (query->commandType == origCmdType &&
				(query->querySource == QSRC_INSTEAD_RULE ||
				 query->querySource == QSRC_QUAL_INSTEAD_RULE))
				lastInstead = query;
		}
	}

	if (!foundOriginalQuery && lastInstead != NULL)
		lastInstead->canSetTag = true;

	return results;
}
