/*-------------------------------------------------------------------------
 *
 * rewriteHandler.c
 *		Primary module of query rewriter.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/rewrite/rewriteHandler.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "foreign/fdwapi.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "parser/analyze.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"


/* We use a list of these to detect recursion in RewriteQuery */
typedef struct rewrite_event
{
	Oid			relation;		/* OID of relation having rules */
	CmdType		event;			/* type of rule being fired */
} rewrite_event;

static bool acquireLocksOnSubLinks(Node *node, void *context);
static Query *rewriteRuleAction(Query *parsetree,
				  Query *rule_action,
				  Node *rule_qual,
				  int rt_index,
				  CmdType event,
				  bool *returning_flag);
static List *adjustJoinTreeList(Query *parsetree, bool removert, int rt_index);
static void rewriteTargetListIU(Query *parsetree, Relation target_relation,
					List **attrno_list);
static TargetEntry *process_matched_tle(TargetEntry *src_tle,
					TargetEntry *prior_tle,
					const char *attrName);
static Node *get_assignment_input(Node *node);
static void rewriteValuesRTE(RangeTblEntry *rte, Relation target_relation,
				 List *attrnos);
static void rewriteTargetListUD(Query *parsetree, RangeTblEntry *target_rte,
					Relation target_relation);
static void markQueryForLocking(Query *qry, Node *jtnode,
				  LockClauseStrength strength, bool noWait, bool pushedDown);
static List *matchLocks(CmdType event, RuleLock *rulelocks,
		   int varno, Query *parsetree);
static Query *fireRIRrules(Query *parsetree, List *activeRIRs,
			 bool forUpdatePushedDown);


/*
 * AcquireRewriteLocks -
 *	  Acquire suitable locks on all the relations mentioned in the Query.
 *	  These locks will ensure that the relation schemas don't change under us
 *	  while we are rewriting and planning the query.
 *
 * forUpdatePushedDown indicates that a pushed-down FOR [KEY] UPDATE/SHARE applies
 * to the current subquery, requiring all rels to be opened with RowShareLock.
 * This should always be false at the start of the recursion.
 *
 * A secondary purpose of this routine is to fix up JOIN RTE references to
 * dropped columns (see details below).  Because the RTEs are modified in
 * place, it is generally appropriate for the caller of this routine to have
 * first done a copyObject() to make a writable copy of the querytree in the
 * current memory context.
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
AcquireRewriteLocks(Query *parsetree, bool forUpdatePushedDown)
{
	ListCell   *l;
	int			rt_index;

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
				 * release it until end of transaction. This protects the
				 * rewriter and planner against schema changes mid-query.
				 *
				 * If the relation is the query's result relation, then we
				 * need RowExclusiveLock.  Otherwise, check to see if the
				 * relation is accessed FOR [KEY] UPDATE/SHARE or not.	We
				 * can't just grab AccessShareLock because then the executor
				 * would be trying to upgrade the lock, leading to possible
				 * deadlocks.
				 */
				if (rt_index == parsetree->resultRelation)
					lockmode = RowExclusiveLock;
				else if (forUpdatePushedDown ||
						 get_parse_rowmark(parsetree, rt_index) != NULL)
					lockmode = RowShareLock;
				else
					lockmode = AccessShareLock;

				rel = heap_open(rte->relid, lockmode);

				/*
				 * While we have the relation open, update the RTE's relkind,
				 * just in case it changed since this rule was made.
				 */
				rte->relkind = rel->rd_rel->relkind;

				heap_close(rel, NoLock);
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
						 * order the planner builds things in.	So we already
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

		AcquireRewriteLocks((Query *) cte->ctequery, false);
	}

	/*
	 * Recurse into sublink subqueries, too.  But we already did the ones in
	 * the rtable and cteList.
	 */
	if (parsetree->hasSubLinks)
		query_tree_walker(parsetree, acquireLocksOnSubLinks, NULL,
						  QTW_IGNORE_RC_SUBQUERIES);
}

/*
 * Walker to find sublink subqueries for AcquireRewriteLocks
 */
static bool
acquireLocksOnSubLinks(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
	{
		SubLink    *sub = (SubLink *) node;

		/* Do what we came for */
		AcquireRewriteLocks((Query *) sub->subselect, false);
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
 *	*returning_flag - set TRUE if we rewrite RETURNING clause in rule_action
 *					(must be initialized to FALSE)
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

	/*
	 * Make modifiable copies of rule action and qual (what we're passed are
	 * the stored versions in the relcache; don't touch 'em!).
	 */
	rule_action = (Query *) copyObject(rule_action);
	rule_qual = (Node *) copyObject(rule_qual);

	/*
	 * Acquire necessary locks and fix any deleted JOIN RTE entries.
	 */
	AcquireRewriteLocks(rule_action, false);
	(void) acquireLocksOnSubLinks(rule_qual, NULL);

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
	 * Generate expanded rtable consisting of main parsetree's rtable plus
	 * rule action's rtable; this becomes the complete rtable for the rule
	 * action.	Some of the entries may be unused after we finish rewriting,
	 * but we leave them all in place for two reasons:
	 *
	 * We'd have a much harder job to adjust the query's varnos if we
	 * selectively removed RT entries.
	 *
	 * If the rule is INSTEAD, then the original query won't be executed at
	 * all, and so its rtable must be preserved so that the executor will do
	 * the correct permissions checks on it.
	 *
	 * RT entries that are not referenced in the completed jointree will be
	 * ignored by the planner, so they do not affect query semantics.  But any
	 * permissions checks specified in them will be applied during executor
	 * startup (see ExecCheckRTEPerms()).  This allows us to check that the
	 * caller has, say, insert-permission on a view, when the view is not
	 * semantically referenced at all in the resulting query.
	 *
	 * When a rule is not INSTEAD, the permissions checks done on its copied
	 * RT entries will be redundant with those done during execution of the
	 * original query, but we don't bother to treat that case differently.
	 *
	 * NOTE: because planner will destructively alter rtable, we must ensure
	 * that rule action's rtable is separate and shares no substructure with
	 * the main rtable.  Hence do a deep copy here.
	 */
	sub_action->rtable = list_concat((List *) copyObject(parsetree->rtable),
									 sub_action->rtable);

	/*
	 * There could have been some SubLinks in parsetree's rtable, in which
	 * case we'd better mark the sub_action correctly.
	 */
	if (parsetree->hasSubLinks && !sub_action->hasSubLinks)
	{
		ListCell   *lc;

		foreach(lc, parsetree->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

			switch (rte->rtekind)
			{
				case RTE_FUNCTION:
					sub_action->hasSubLinks =
						checkExprHasSubLink(rte->funcexpr);
					break;
				case RTE_VALUES:
					sub_action->hasSubLinks =
						checkExprHasSubLink((Node *) rte->values_lists);
					break;
				default:
					/* other RTE types don't contain bare expressions */
					break;
			}
			if (sub_action->hasSubLinks)
				break;			/* no need to keep scanning rtable */
		}
	}

	/*
	 * Each rule action's jointree should be the main parsetree's jointree
	 * plus that rule's jointree, but usually *without* the original rtindex
	 * that we're replacing (if present, which it won't be for INSERT). Note
	 * that if the rule action refers to OLD, its jointree will add a
	 * reference to rt_index.  If the rule action doesn't refer to OLD, but
	 * either the rule_qual or the user query quals do, then we need to keep
	 * the original rtindex in the jointree to provide data for the quals.	We
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
			 * good at all, because the jointree is dummy.	(Perhaps someday
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
		ListCell   *lc;

		/*
		 * Annoying implementation restriction: because CTEs are identified by
		 * name within a cteList, we can't merge a CTE from the original query
		 * if it has the same name as any CTE in the rule action.
		 *
		 * This could possibly be fixed by using some sort of internally
		 * generated ID, instead of names, to link CTE RTEs to their CTEs.
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
									  REPLACEVARS_REPORT_ERROR,
									  0,
									  &rule_action->hasSubLinks);

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
				newjointree = list_delete_ptr(newjointree, rtr);

				/*
				 * foreach is safe because we exit loop after list_delete...
				 */
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
 * 2. For an UPDATE on a view, add tlist entries for any unassigned-to
 * attributes, assigning them their old values.  These will later get
 * expanded to the output values of the view.  (This is equivalent to what
 * the planner's expand_targetlist() will do for UPDATE on a regular table,
 * but it's more convenient to do it here while we still have easy access
 * to the view's original RT index.)
 *
 * 3. Merge multiple entries for the same target attribute, or declare error
 * if we can't.  Multiple entries are only allowed for INSERT/UPDATE of
 * portions of an array or record field, for example
 *			UPDATE table SET foo[2] = 42, foo[4] = 43;
 * We can merge such operations into a single assignment op.  Essentially,
 * the expression we want to produce in this case is like
 *		foo = array_set(array_set(foo, 2, 42), 4, 43)
 *
 * 4. Sort the tlist into standard order: non-junk fields in order by resno,
 * then junk fields (these in no particular order).
 *
 * We must do items 1,2,3 before firing rewrite rules, else rewritten
 * references to NEW.foo will produce wrong or incomplete results.	Item 4
 * is not needed for rewriting, but will be needed by the planner, and we
 * can do it essentially for free while handling the other items.
 *
 * If attrno_list isn't NULL, we return an additional output besides the
 * rewritten targetlist: an integer list of the assigned-to attnums, in
 * order of the original tlist's non-junk entries.  This is needed for
 * processing VALUES RTEs.
 */
static void
rewriteTargetListIU(Query *parsetree, Relation target_relation,
					List **attrno_list)
{
	CmdType		commandType = parsetree->commandType;
	TargetEntry **new_tles;
	List	   *new_tlist = NIL;
	List	   *junk_tlist = NIL;
	Form_pg_attribute att_tup;
	int			attrno,
				next_junk_attrno,
				numattrs;
	ListCell   *temp;

	if (attrno_list)			/* initialize optional result list */
		*attrno_list = NIL;

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

	foreach(temp, parsetree->targetList)
	{
		TargetEntry *old_tle = (TargetEntry *) lfirst(temp);

		if (!old_tle->resjunk)
		{
			/* Normal attr: stash it into new_tles[] */
			attrno = old_tle->resno;
			if (attrno < 1 || attrno > numattrs)
				elog(ERROR, "bogus resno %d in targetlist", attrno);
			att_tup = target_relation->rd_att->attrs[attrno - 1];

			/* put attrno into attrno_list even if it's dropped */
			if (attrno_list)
				*attrno_list = lappend_int(*attrno_list, attrno);

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

		att_tup = target_relation->rd_att->attrs[attrno - 1];

		/* We can (and must) ignore deleted attributes */
		if (att_tup->attisdropped)
			continue;

		/*
		 * Handle the two cases where we need to insert a default expression:
		 * it's an INSERT and there's no tlist entry for the column, or the
		 * tlist entry is a DEFAULT placeholder node.
		 */
		if ((new_tle == NULL && commandType == CMD_INSERT) ||
			(new_tle && new_tle->expr && IsA(new_tle->expr, SetToDefault)))
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
				{
					new_expr = (Node *) makeConst(att_tup->atttypid,
												  -1,
												  att_tup->attcollation,
												  att_tup->attlen,
												  (Datum) 0,
												  true, /* isnull */
												  att_tup->attbyval);
					/* this is to catch a NOT NULL domain constraint */
					new_expr = coerce_to_domain(new_expr,
												InvalidOid, -1,
												att_tup->atttypid,
												COERCE_IMPLICIT_CAST,
												-1,
												false,
												false);
				}
			}

			if (new_expr)
				new_tle = makeTargetEntry((Expr *) new_expr,
										  attrno,
										  pstrdup(NameStr(att_tup->attname)),
										  false);
		}

		/*
		 * For an UPDATE on a view, provide a dummy entry whenever there is no
		 * explicit assignment.
		 */
		if (new_tle == NULL && commandType == CMD_UPDATE &&
			target_relation->rd_rel->relkind == RELKIND_VIEW)
		{
			Node	   *new_expr;

			new_expr = (Node *) makeVar(parsetree->resultRelation,
										attrno,
										att_tup->atttypid,
										att_tup->atttypmod,
										att_tup->attcollation,
										0);

			new_tle = makeTargetEntry((Expr *) new_expr,
									  attrno,
									  pstrdup(NameStr(att_tup->attname)),
									  false);
		}

		if (new_tle)
			new_tlist = lappend(new_tlist, new_tle);
	}

	pfree(new_tles);

	parsetree->targetList = list_concat(new_tlist, junk_tlist);
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
	 * Multiple assignments to same attribute.	Allow only if all are
	 * FieldStore or ArrayRef assignment operations.  This is a bit
	 * tricky because what we may actually be looking at is a nest of
	 * such nodes; consider
	 *		UPDATE tab SET col.fld1.subfld1 = x, col.fld2.subfld2 = y
	 * The two expressions produced by the parser will look like
	 *		FieldStore(col, fld1, FieldStore(placeholder, subfld1, x))
	 *		FieldStore(col, fld2, FieldStore(placeholder, subfld2, x))
	 * However, we can ignore the substructure and just consider the top
	 * FieldStore or ArrayRef from each assignment, because it works to
	 * combine these as
	 *		FieldStore(FieldStore(col, fld1,
	 *							  FieldStore(placeholder, subfld1, x)),
	 *				   fld2, FieldStore(placeholder, subfld2, x))
	 * Note the leftmost expression goes on the inside so that the
	 * assignments appear to occur left-to-right.
	 *
	 * For FieldStore, instead of nesting we can generate a single
	 * FieldStore with multiple target fields.	We must nest when
	 * ArrayRefs are involved though.
	 *----------
	 */
	src_expr = (Node *) src_tle->expr;
	prior_expr = (Node *) prior_tle->expr;
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
				list_concat(list_copy(((FieldStore *) prior_expr)->newvals),
							list_copy(((FieldStore *) src_expr)->newvals));
			fstore->fieldnums =
				list_concat(list_copy(((FieldStore *) prior_expr)->fieldnums),
							list_copy(((FieldStore *) src_expr)->fieldnums));
		}
		else
		{
			/* general case, just nest 'em */
			memcpy(fstore, src_expr, sizeof(FieldStore));
			fstore->arg = (Expr *) prior_expr;
		}
		newexpr = (Node *) fstore;
	}
	else if (IsA(src_expr, ArrayRef))
	{
		ArrayRef   *aref = makeNode(ArrayRef);

		memcpy(aref, src_expr, sizeof(ArrayRef));
		aref->refexpr = (Expr *) prior_expr;
		newexpr = (Node *) aref;
	}
	else
	{
		elog(ERROR, "cannot happen");
		newexpr = NULL;
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
	else if (IsA(node, ArrayRef))
	{
		ArrayRef   *aref = (ArrayRef *) node;

		if (aref->refassgnexpr == NULL)
			return NULL;
		return (Node *) aref->refexpr;
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
	Form_pg_attribute att_tup = rd_att->attrs[attrno - 1];
	Oid			atttype = att_tup->atttypid;
	int32		atttypmod = att_tup->atttypmod;
	Node	   *expr = NULL;
	Oid			exprtype;

	/*
	 * Scan to see if relation has a default for this column.
	 */
	if (rd_att->constr && rd_att->constr->num_defval > 0)
	{
		AttrDefault *defval = rd_att->constr->defval;
		int			ndef = rd_att->constr->num_defval;

		while (--ndef >= 0)
		{
			if (attrno == defval[ndef].adnum)
			{
				/*
				 * Found it, convert string representation to node tree.
				 */
				expr = stringToNode(defval[ndef].adbin);
				break;
			}
		}
	}

	if (expr == NULL)
	{
		/*
		 * No per-column default, so look for a default for the type itself.
		 */
		expr = get_typdefault(atttype);
	}

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
 * When processing INSERT ... VALUES with a VALUES RTE (ie, multiple VALUES
 * lists), we have to replace any DEFAULT items in the VALUES lists with
 * the appropriate default expressions.  The other aspects of targetlist
 * rewriting need be applied only to the query's targetlist proper.
 *
 * Note that we currently can't support subscripted or field assignment
 * in the multi-VALUES case.  The targetlist will contain simple Vars
 * referencing the VALUES RTE, and therefore process_matched_tle() will
 * reject any such attempt with "multiple assignments to same column".
 */
static void
rewriteValuesRTE(RangeTblEntry *rte, Relation target_relation, List *attrnos)
{
	List	   *newValues;
	ListCell   *lc;

	/*
	 * Rebuilding all the lists is a pretty expensive proposition in a big
	 * VALUES list, and it's a waste of time if there aren't any DEFAULT
	 * placeholders.  So first scan to see if there are any.
	 */
	if (!searchForDefault(rte))
		return;					/* nothing to do */

	/* Check list lengths (we can assume all the VALUES sublists are alike) */
	Assert(list_length(attrnos) == list_length(linitial(rte->values_lists)));

	newValues = NIL;
	foreach(lc, rte->values_lists)
	{
		List	   *sublist = (List *) lfirst(lc);
		List	   *newList = NIL;
		ListCell   *lc2;
		ListCell   *lc3;

		forboth(lc2, sublist, lc3, attrnos)
		{
			Node	   *col = (Node *) lfirst(lc2);
			int			attrno = lfirst_int(lc3);

			if (IsA(col, SetToDefault))
			{
				Form_pg_attribute att_tup;
				Node	   *new_expr;

				att_tup = target_relation->rd_att->attrs[attrno - 1];

				if (!att_tup->attisdropped)
					new_expr = build_column_default(target_relation, attrno);
				else
					new_expr = NULL;	/* force a NULL if dropped */

				/*
				 * If there is no default (ie, default is effectively NULL),
				 * we've got to explicitly set the column to NULL.
				 */
				if (!new_expr)
				{
					new_expr = (Node *) makeConst(att_tup->atttypid,
												  -1,
												  att_tup->attcollation,
												  att_tup->attlen,
												  (Datum) 0,
												  true, /* isnull */
												  att_tup->attbyval);
					/* this is to catch a NOT NULL domain constraint */
					new_expr = coerce_to_domain(new_expr,
												InvalidOid, -1,
												att_tup->atttypid,
												COERCE_IMPLICIT_CAST,
												-1,
												false,
												false);
				}
				newList = lappend(newList, new_expr);
			}
			else
				newList = lappend(newList, col);
		}
		newValues = lappend(newValues, newList);
	}
	rte->values_lists = newValues;
}


/*
 * rewriteTargetListUD - rewrite UPDATE/DELETE targetlist as needed
 *
 * This function adds a "junk" TLE that is needed to allow the executor to
 * find the original row for the update or delete.	When the target relation
 * is a regular table, the junk TLE emits the ctid attribute of the original
 * row.  When the target relation is a view, there is no ctid, so we instead
 * emit a whole-row Var that will contain the "old" values of the view row.
 * If it's a foreign table, we let the FDW decide what to add.
 *
 * For UPDATE queries, this is applied after rewriteTargetListIU.  The
 * ordering isn't actually critical at the moment.
 */
static void
rewriteTargetListUD(Query *parsetree, RangeTblEntry *target_rte,
					Relation target_relation)
{
	Var		   *var;
	const char *attrname;
	TargetEntry *tle;

	if (target_relation->rd_rel->relkind == RELKIND_RELATION ||
		target_relation->rd_rel->relkind == RELKIND_MATVIEW)
	{
		/*
		 * Emit CTID so that executor can find the row to update or delete.
		 */
		var = makeVar(parsetree->resultRelation,
					  SelfItemPointerAttributeNumber,
					  TIDOID,
					  -1,
					  InvalidOid,
					  0);

		attrname = "ctid";
	}
	else if (target_relation->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		/*
		 * Let the foreign table's FDW add whatever junk TLEs it wants.
		 */
		FdwRoutine *fdwroutine;

		fdwroutine = GetFdwRoutineForRelation(target_relation, false);

		if (fdwroutine->AddForeignUpdateTargets != NULL)
			fdwroutine->AddForeignUpdateTargets(parsetree, target_rte,
												target_relation);

		return;
	}
	else
	{
		/*
		 * Emit whole-row Var so that executor will have the "old" view row to
		 * pass to the INSTEAD OF trigger.
		 */
		var = makeWholeRowVar(target_rte,
							  parsetree->resultRelation,
							  0,
							  false);

		attrname = "wholerow";
	}

	tle = makeTargetEntry((Expr *) var,
						  list_length(parsetree->targetList) + 1,
						  pstrdup(attrname),
						  true);

	parsetree->targetList = lappend(parsetree->targetList, tle);
}


/*
 * matchLocks -
 *	  match the list of locks and returns the matching rules
 */
static List *
matchLocks(CmdType event,
		   RuleLock *rulelocks,
		   int varno,
		   Query *parsetree)
{
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

		/*
		 * Suppress ON INSERT/UPDATE/DELETE rules that are disabled or
		 * configured to not fire during the current sessions replication
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
			else	/* ORIGIN or LOCAL ROLE */
			{
				if (oneLock->enabled == RULE_FIRES_ON_REPLICA ||
					oneLock->enabled == RULE_DISABLED)
					continue;
			}
		}

		if (oneLock->event == event)
		{
			if (parsetree->commandType != CMD_SELECT ||
				(oneLock->attrno == -1 ?
				 rangeTableEntry_used((Node *) parsetree, varno, 0) :
				 attribute_used((Node *) parsetree,
								varno, oneLock->attrno, 0)))
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
				  bool relation_level,
				  Relation relation,
				  List *activeRIRs,
				  bool forUpdatePushedDown)
{
	Query	   *rule_action;
	RangeTblEntry *rte,
			   *subrte;
	RowMarkClause *rc;

	if (list_length(rule->actions) != 1)
		elog(ERROR, "expected just one rule action");
	if (rule->qual != NULL)
		elog(ERROR, "cannot handle qualified ON SELECT rule");
	if (!relation_level)
		elog(ERROR, "cannot handle per-attribute ON SELECT rule");

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
		 * For UPDATE/DELETE, we need to expand the view so as to have source
		 * data for the operation.	But we also need an unmodified RTE to
		 * serve as the target.  So, copy the RTE and add the copy to the
		 * rangetable.	Note that the copy does not get added to the jointree.
		 * Also note that there's a hack in fireRIRrules to avoid calling this
		 * function again when it arrives at the copied RTE.
		 */
		if (parsetree->commandType == CMD_INSERT)
			return parsetree;
		else if (parsetree->commandType == CMD_UPDATE ||
				 parsetree->commandType == CMD_DELETE)
		{
			RangeTblEntry *newrte;

			rte = rt_fetch(rt_index, parsetree->rtable);
			newrte = copyObject(rte);
			parsetree->rtable = lappend(parsetree->rtable, newrte);
			parsetree->resultRelation = list_length(parsetree->rtable);

			/*
			 * There's no need to do permissions checks twice, so wipe out the
			 * permissions info for the original RTE (we prefer to keep the
			 * bits set on the result RTE).
			 */
			rte->requiredPerms = 0;
			rte->checkAsUser = InvalidOid;
			rte->selectedCols = NULL;
			rte->modifiedCols = NULL;

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

			/* Now, continue with expanding the original view RTE */
		}
		else
			elog(ERROR, "unrecognized commandType: %d",
				 (int) parsetree->commandType);
	}

	/*
	 * If FOR [KEY] UPDATE/SHARE of view, be sure we get right initial lock on
	 * the relations it references.
	 */
	rc = get_parse_rowmark(parsetree, rt_index);
	forUpdatePushedDown |= (rc != NULL);

	/*
	 * Make a modifiable copy of the view query, and acquire needed locks on
	 * the relations it mentions.
	 */
	rule_action = copyObject(linitial(rule->actions));

	AcquireRewriteLocks(rule_action, forUpdatePushedDown);

	/*
	 * Recursively expand any view references inside the view.
	 */
	rule_action = fireRIRrules(rule_action, activeRIRs, forUpdatePushedDown);

	/*
	 * Now, plug the view query in as a subselect, replacing the relation's
	 * original RTE.
	 */
	rte = rt_fetch(rt_index, parsetree->rtable);

	rte->rtekind = RTE_SUBQUERY;
	rte->relid = InvalidOid;
	rte->security_barrier = RelationIsSecurityView(relation);
	rte->subquery = rule_action;
	rte->inh = false;			/* must not be set for a subquery */

	/*
	 * We move the view's permission check data down to its rangetable. The
	 * checks will actually be done against the OLD entry therein.
	 */
	subrte = rt_fetch(PRS2_OLD_VARNO, rule_action->rtable);
	Assert(subrte->relid == relation->rd_id);
	subrte->requiredPerms = rte->requiredPerms;
	subrte->checkAsUser = rte->checkAsUser;
	subrte->selectedCols = rte->selectedCols;
	subrte->modifiedCols = rte->modifiedCols;

	rte->requiredPerms = 0;		/* no permission check on subquery itself */
	rte->checkAsUser = InvalidOid;
	rte->selectedCols = NULL;
	rte->modifiedCols = NULL;

	/*
	 * If FOR [KEY] UPDATE/SHARE of view, mark all the contained tables as
	 * implicit FOR [KEY] UPDATE/SHARE, the same as the parser would have done
	 * if the view's subquery had been written out explicitly.
	 *
	 * Note: we don't consider forUpdatePushedDown here; such marks will be
	 * made by recursing from the upper level in markQueryForLocking.
	 */
	if (rc != NULL)
		markQueryForLocking(rule_action, (Node *) rule_action->jointree,
							rc->strength, rc->noWait, true);

	return parsetree;
}

/*
 * Recursively mark all relations used by a view as FOR [KEY] UPDATE/SHARE.
 *
 * This may generate an invalid query, eg if some sub-query uses an
 * aggregate.  We leave it to the planner to detect that.
 *
 * NB: this must agree with the parser's transformLockingClause() routine.
 * However, unlike the parser we have to be careful not to mark a view's
 * OLD and NEW rels for updating.  The best way to handle that seems to be
 * to scan the jointree to determine which rels are used.
 */
static void
markQueryForLocking(Query *qry, Node *jtnode,
					LockClauseStrength strength, bool noWait, bool pushedDown)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		int			rti = ((RangeTblRef *) jtnode)->rtindex;
		RangeTblEntry *rte = rt_fetch(rti, qry->rtable);

		if (rte->rtekind == RTE_RELATION)
		{
			applyLockingClause(qry, rti, strength, noWait, pushedDown);
			rte->requiredPerms |= ACL_SELECT_FOR_UPDATE;
		}
		else if (rte->rtekind == RTE_SUBQUERY)
		{
			applyLockingClause(qry, rti, strength, noWait, pushedDown);
			/* FOR UPDATE/SHARE of subquery is propagated to subquery's rels */
			markQueryForLocking(rte->subquery, (Node *) rte->subquery->jointree,
								strength, noWait, true);
		}
		/* other RTE types are unaffected by FOR UPDATE */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
			markQueryForLocking(qry, lfirst(l), strength, noWait, pushedDown);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		markQueryForLocking(qry, j->larg, strength, noWait, pushedDown);
		markQueryForLocking(qry, j->rarg, strength, noWait, pushedDown);
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
 * SubLink nodes in-place.	It is caller's responsibility to ensure that
 * no unwanted side-effects occur!
 *
 * This is unlike most of the other routines that recurse into subselects,
 * because we must take control at the SubLink node in order to replace
 * the SubLink's subselect link with the possibly-rewritten subquery.
 */
static bool
fireRIRonSubLink(Node *node, List *activeRIRs)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
	{
		SubLink    *sub = (SubLink *) node;

		/* Do what we came for */
		sub->subselect = (Node *) fireRIRrules((Query *) sub->subselect,
											   activeRIRs, false);
		/* Fall through to process lefthand args of SubLink */
	}

	/*
	 * Do NOT recurse into Query nodes, because fireRIRrules already processed
	 * subselects of subselects for us.
	 */
	return expression_tree_walker(node, fireRIRonSubLink,
								  (void *) activeRIRs);
}


/*
 * fireRIRrules -
 *	Apply all RIR rules on each rangetable entry in a query
 */
static Query *
fireRIRrules(Query *parsetree, List *activeRIRs, bool forUpdatePushedDown)
{
	int			origResultRelation = parsetree->resultRelation;
	int			rt_index;
	ListCell   *lc;

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
			rte->subquery = fireRIRrules(rte->subquery, activeRIRs,
										 (forUpdatePushedDown ||
							get_parse_rowmark(parsetree, rt_index) != NULL));
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
		rel = heap_open(rte->relid, NoLock);

		/*
		 * Collect the RIR rules that we must apply
		 */
		rules = rel->rd_rules;
		if (rules == NULL)
		{
			heap_close(rel, NoLock);
			continue;
		}
		locks = NIL;
		for (i = 0; i < rules->numLocks; i++)
		{
			rule = rules->rules[i];
			if (rule->event != CMD_SELECT)
				continue;

			if (rule->attrno > 0)
			{
				/* per-attr rule; do we need it? */
				if (!attribute_used((Node *) parsetree, rt_index,
									rule->attrno, 0))
					continue;
			}

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
			activeRIRs = lcons_oid(RelationGetRelid(rel), activeRIRs);

			foreach(l, locks)
			{
				rule = lfirst(l);

				parsetree = ApplyRetrieveRule(parsetree,
											  rule,
											  rt_index,
											  rule->attrno == -1,
											  rel,
											  activeRIRs,
											  forUpdatePushedDown);
			}

			activeRIRs = list_delete_first(activeRIRs);
		}

		heap_close(rel, NoLock);
	}

	/* Recurse into subqueries in WITH */
	foreach(lc, parsetree->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		cte->ctequery = (Node *)
			fireRIRrules((Query *) cte->ctequery, activeRIRs, false);
	}

	/*
	 * Recurse into sublink subqueries, too.  But we already did the ones in
	 * the rtable and cteList.
	 */
	if (parsetree->hasSubLinks)
		query_tree_walker(parsetree, fireRIRonSubLink, (void *) activeRIRs,
						  QTW_IGNORE_RC_SUBQUERIES);

	return parsetree;
}


/*
 * Modify the given query by adding 'AND rule_qual IS NOT TRUE' to its
 * qualification.  This is used to generate suitable "else clauses" for
 * conditional INSTEAD rules.  (Unfortunately we must use "x IS NOT TRUE",
 * not just "NOT x" which the planner is much smarter about, else we will
 * do the wrong thing when the qual evaluates to NULL.)
 *
 * The rule_qual may contain references to OLD or NEW.	OLD references are
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
	Node	   *new_qual = (Node *) copyObject(rule_qual);

	/*
	 * In case there are subqueries in the qual, acquire necessary locks and
	 * fix any deleted JOIN RTE entries.  (This is somewhat redundant with
	 * rewriteRuleAction, but not entirely ... consider restructuring so that
	 * we only need to process the qual this way once.)
	 */
	(void) acquireLocksOnSubLinks(new_qual, NULL);

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
 *	*instead_flag - set TRUE if any unqualified INSTEAD rule is found
 *					(must be initialized to FALSE)
 *	*returning_flag - set TRUE if we rewrite RETURNING clause in any rule
 *					(must be initialized to FALSE)
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
 * query if we find either qualified or unqualified INSTEAD rules.	If
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
			rule_action->canSetTag = false;		/* might change later */

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
 */
static Query *
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
 * be folded into view_is_auto_updatable because it's not an error condition.
 */
static bool
view_has_instead_trigger(Relation view, CmdType event)
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
		default:
			elog(ERROR, "unrecognized CmdType: %d", (int) event);
			break;
	}
	return false;
}


/*
 * view_is_auto_updatable -
 *	  Test if the specified view can be automatically updated. This will
 *	  either return NULL (if the view can be updated) or a message string
 *	  giving the reason that it cannot be.
 *
 * Caller must have verified that relation is a view!
 *
 * Note that the checks performed here are local to this view.	We do not
 * check whether the view's underlying base relation is updatable; that
 * will be dealt with in later, recursive processing.
 *
 * Also note that we don't check for INSTEAD triggers or rules here; those
 * also prevent auto-update, but they must be checked for by the caller.
 */
static const char *
view_is_auto_updatable(Relation view)
{
	Query	   *viewquery = get_view_query(view);
	RangeTblRef *rtr;
	RangeTblEntry *base_rte;
	Bitmapset  *bms;
	ListCell   *cell;

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
	 * In addition we impose these constraints, involving features that are
	 * not part of SQL-92:
	 *	- No CTEs (WITH clauses).
	 *	- No OFFSET or LIMIT clauses (this matches a SQL:2008 restriction).
	 *	- No system columns (including whole-row references) in the tlist.
	 *
	 * Note that we do these checks without recursively expanding the view.
	 * If the base relation is a view, we'll recursively deal with it later.
	 *----------
	 */
	if (viewquery->distinctClause != NIL)
		return gettext_noop("Views containing DISTINCT are not automatically updatable.");

	if (viewquery->groupClause != NIL)
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
	 * For now, we also don't support security-barrier views, because of the
	 * difficulty of keeping upper-level qual expressions away from
	 * lower-level data.  This might get relaxed in future.
	 */
	if (RelationIsSecurityView(view))
		return gettext_noop("Security-barrier views are not automatically updatable.");

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
		 base_rte->relkind != RELKIND_VIEW))
		return gettext_noop("Views that do not select from a single table or view are not automatically updatable.");

	/*
	 * The view's targetlist entries should all be Vars referring to user
	 * columns of the base relation, and no two should refer to the same
	 * column.
	 *
	 * Note however that we should ignore resjunk entries.	This proviso is
	 * relevant because ORDER BY is not disallowed, and we shouldn't reject a
	 * view defined like "SELECT * FROM t ORDER BY a+b".
	 */
	bms = NULL;
	foreach(cell, viewquery->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(cell);
		Var		   *var = (Var *) tle->expr;

		if (tle->resjunk)
			continue;

		if (!IsA(var, Var) ||
			var->varno != rtr->rtindex ||
			var->varlevelsup != 0)
			return gettext_noop("Views that return columns that are not columns of their base relation are not automatically updatable.");

		if (var->varattno < 0)
			return gettext_noop("Views that return system columns are not automatically updatable.");

		if (var->varattno == 0)
			return gettext_noop("Views that return whole-row references are not automatically updatable.");

		if (bms_is_member(var->varattno, bms))
			return gettext_noop("Views that return the same column more than once are not automatically updatable.");

		bms = bms_add_member(bms, var->varattno);
	}
	bms_free(bms);				/* just for cleanliness */

	return NULL;				/* the view is simply updatable */
}


/*
 * relation_is_updatable - determine which update events the specified
 * relation supports.
 *
 * This is used for the information_schema views, which have separate concepts
 * of "updatable" and "trigger updatable".	A relation is "updatable" if it
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
 * the INSERT, UPDATE and DELETE operations are supported.	(We do it this way
 * so that we can test for UPDATE plus DELETE support in a single call.)
 */
int
relation_is_updatable(Oid reloid, bool include_triggers)
{
	int			events = 0;
	Relation	rel;
	RuleLock   *rulelocks;

#define ALL_EVENTS ((1 << CMD_INSERT) | (1 << CMD_UPDATE) | (1 << CMD_DELETE))

	rel = try_relation_open(reloid, AccessShareLock);

	/*
	 * If the relation doesn't exist, return zero rather than throwing an
	 * error.  This is helpful since scanning an information_schema view under
	 * MVCC rules can result in referencing rels that were just deleted
	 * according to a SnapshotNow probe.
	 */
	if (rel == NULL)
		return 0;

	/* If the relation is a table, it is always updatable */
	if (rel->rd_rel->relkind == RELKIND_RELATION)
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
	if (rel->rd_rel->relkind == RELKIND_VIEW &&
		view_is_auto_updatable(rel) == NULL)
	{
		Query	   *viewquery;
		RangeTblRef *rtr;
		RangeTblEntry *base_rte;
		Oid			baseoid;

		/* The base relation must also be updatable */
		viewquery = get_view_query(rel);
		rtr = (RangeTblRef *) linitial(viewquery->jointree->fromlist);
		base_rte = rt_fetch(rtr->rtindex, viewquery->rtable);
		Assert(base_rte->rtekind == RTE_RELATION);

		if (base_rte->relkind == RELKIND_RELATION)
		{
			/* Tables are always updatable */
			relation_close(rel, AccessShareLock);
			return ALL_EVENTS;
		}
		else
		{
			/* Do a recursive check for any other kind of base relation */
			baseoid = base_rte->relid;
			relation_close(rel, AccessShareLock);
			return relation_is_updatable(baseoid, include_triggers);
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
 * The targetlist is expected to be a list of plain Vars of the underlying
 * relation (as per the checks above in view_is_auto_updatable).
 */
static Bitmapset *
adjust_view_column_set(Bitmapset *cols, List *targetlist)
{
	Bitmapset  *result = NULL;
	Bitmapset  *tmpcols;
	AttrNumber	col;

	tmpcols = bms_copy(cols);
	while ((col = bms_first_member(tmpcols)) >= 0)
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
				TargetEntry *tle = (TargetEntry *) lfirst(lc);
				Var		   *var;

				if (tle->resjunk)
					continue;
				var = (Var *) tle->expr;
				Assert(IsA(var, Var));
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
	bms_free(tmpcols);

	return result;
}


/*
 * rewriteTargetView -
 *	  Attempt to rewrite a query where the target relation is a view, so that
 *	  the view's base relation becomes the target relation.
 *
 * Note that the base relation here may itself be a view, which may or may not
 * have INSTEAD OF triggers or rules to handle the update.	That is handled by
 * the recursion in RewriteQuery.
 */
static Query *
rewriteTargetView(Query *parsetree, Relation view)
{
	const char *auto_update_detail;
	Query	   *viewquery;
	RangeTblRef *rtr;
	int			base_rt_index;
	int			new_rt_index;
	RangeTblEntry *base_rte;
	RangeTblEntry *view_rte;
	RangeTblEntry *new_rte;
	Relation	base_rel;
	List	   *view_targetlist;
	ListCell   *lc;

	/* The view must be simply updatable, else fail */
	auto_update_detail = view_is_auto_updatable(view);
	if (auto_update_detail)
	{
		/* messages here should match execMain.c's CheckValidResultRel */
		switch (parsetree->commandType)
		{
			case CMD_INSERT:
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("cannot insert into view \"%s\"",
								RelationGetRelationName(view)),
						 errdetail_internal("%s", _(auto_update_detail)),
						 errhint("To enable inserting into the view, provide an INSTEAD OF INSERT trigger or an unconditional ON INSERT DO INSTEAD rule.")));
				break;
			case CMD_UPDATE:
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("cannot update view \"%s\"",
								RelationGetRelationName(view)),
						 errdetail_internal("%s", _(auto_update_detail)),
						 errhint("To enable updating the view, provide an INSTEAD OF UPDATE trigger or an unconditional ON UPDATE DO INSTEAD rule.")));
				break;
			case CMD_DELETE:
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("cannot delete from view \"%s\"",
								RelationGetRelationName(view)),
						 errdetail_internal("%s", _(auto_update_detail)),
						 errhint("To enable deleting from the view, provide an INSTEAD OF DELETE trigger or an unconditional ON DELETE DO INSTEAD rule.")));
				break;
			default:
				elog(ERROR, "unrecognized CmdType: %d",
					 (int) parsetree->commandType);
				break;
		}
	}

	/* Locate RTE describing the view in the outer query */
	view_rte = rt_fetch(parsetree->resultRelation, parsetree->rtable);

	/*
	 * If we get here, view_is_auto_updatable() has verified that the view
	 * contains a single base relation.
	 */
	viewquery = get_view_query(view);

	Assert(list_length(viewquery->jointree->fromlist) == 1);
	rtr = (RangeTblRef *) linitial(viewquery->jointree->fromlist);
	Assert(IsA(rtr, RangeTblRef));

	base_rt_index = rtr->rtindex;
	base_rte = rt_fetch(base_rt_index, viewquery->rtable);
	Assert(base_rte->rtekind == RTE_RELATION);

	/*
	 * Up to now, the base relation hasn't been touched at all in our query.
	 * We need to acquire lock on it before we try to do anything with it.
	 * (The subsequent recursive call of RewriteQuery will suppose that we
	 * already have the right lock!)  Since it will become the query target
	 * relation, RowExclusiveLock is always the right thing.
	 */
	base_rel = heap_open(base_rte->relid, RowExclusiveLock);

	/*
	 * While we have the relation open, update the RTE's relkind, just in case
	 * it changed since this view was made (cf. AcquireRewriteLocks).
	 */
	base_rte->relkind = base_rel->rd_rel->relkind;

	heap_close(base_rel, NoLock);

	/*
	 * Create a new target RTE describing the base relation, and add it to the
	 * outer query's rangetable.  (What's happening in the next few steps is
	 * very much like what the planner would do to "pull up" the view into the
	 * outer query.  Perhaps someday we should refactor things enough so that
	 * we can share code with the planner.)
	 */
	new_rte = (RangeTblEntry *) copyObject(base_rte);
	parsetree->rtable = lappend(parsetree->rtable, new_rte);
	new_rt_index = list_length(parsetree->rtable);

	/*
	 * INSERTs never inherit.  For UPDATE/DELETE, we use the view query's
	 * inheritance flag for the base relation.
	 */
	if (parsetree->commandType == CMD_INSERT)
		new_rte->inh = false;

	/*
	 * Make a copy of the view's targetlist, adjusting its Vars to reference
	 * the new target RTE, ie make their varnos be new_rt_index instead of
	 * base_rt_index.  There can be no Vars for other rels in the tlist, so
	 * this is sufficient to pull up the tlist expressions for use in the
	 * outer query.  The tlist will provide the replacement expressions used
	 * by ReplaceVarsFromTargetList below.
	 */
	view_targetlist = copyObject(viewquery->targetList);

	ChangeVarNodes((Node *) view_targetlist,
				   base_rt_index,
				   new_rt_index,
				   0);

	/*
	 * Mark the new target RTE for the permissions checks that we want to
	 * enforce against the view owner, as distinct from the query caller.  At
	 * the relation level, require the same INSERT/UPDATE/DELETE permissions
	 * that the query caller needs against the view.  We drop the ACL_SELECT
	 * bit that is presumably in new_rte->requiredPerms initially.
	 *
	 * Note: the original view RTE remains in the query's rangetable list.
	 * Although it will be unused in the query plan, we need it there so that
	 * the executor still performs appropriate permissions checks for the
	 * query caller's use of the view.
	 */
	new_rte->checkAsUser = view->rd_rel->relowner;
	new_rte->requiredPerms = view_rte->requiredPerms;

	/*
	 * Now for the per-column permissions bits.
	 *
	 * Initially, new_rte contains selectedCols permission check bits for all
	 * base-rel columns referenced by the view, but since the view is a SELECT
	 * query its modifiedCols is empty.  We set modifiedCols to include all
	 * the columns the outer query is trying to modify, adjusting the column
	 * numbers as needed.  But we leave selectedCols as-is, so the view owner
	 * must have read permission for all columns used in the view definition,
	 * even if some of them are not read by the outer query.  We could try to
	 * limit selectedCols to only columns used in the transformed query, but
	 * that does not correspond to what happens in ordinary SELECT usage of a
	 * view: all referenced columns must have read permission, even if
	 * optimization finds that some of them can be discarded during query
	 * transformation.	The flattening we're doing here is an optional
	 * optimization, too.  (If you are unpersuaded and want to change this,
	 * note that applying adjust_view_column_set to view_rte->selectedCols is
	 * clearly *not* the right answer, since that neglects base-rel columns
	 * used in the view's WHERE quals.)
	 *
	 * This step needs the modified view targetlist, so we have to do things
	 * in this order.
	 */
	Assert(bms_is_empty(new_rte->modifiedCols));
	new_rte->modifiedCols = adjust_view_column_set(view_rte->modifiedCols,
												   view_targetlist);

	/*
	 * For UPDATE/DELETE, rewriteTargetListUD will have added a wholerow junk
	 * TLE for the view to the end of the targetlist, which we no longer need.
	 * Remove it to avoid unnecessary work when we process the targetlist.
	 * Note that when we recurse through rewriteQuery a new junk TLE will be
	 * added to allow the executor to find the proper row in the new target
	 * relation.  (So, if we failed to do this, we might have multiple junk
	 * TLEs with the same name, which would be disastrous.)
	 */
	if (parsetree->commandType != CMD_INSERT)
	{
		TargetEntry *tle = (TargetEntry *) llast(parsetree->targetList);

		Assert(tle->resjunk);
		Assert(IsA(tle->expr, Var) &&
			   ((Var *) tle->expr)->varno == parsetree->resultRelation &&
			   ((Var *) tle->expr)->varattno == 0);
		parsetree->targetList = list_delete_ptr(parsetree->targetList, tle);
	}

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
								  REPLACEVARS_REPORT_ERROR,
								  0,
								  &parsetree->hasSubLinks);

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
	 * columns to be affected.
	 *
	 * Note that this destroys the resno ordering of the targetlist, but that
	 * will be fixed when we recurse through rewriteQuery, which will invoke
	 * rewriteTargetListIU again on the updated targetlist.
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
	}

	/*
	 * For UPDATE/DELETE, pull up any WHERE quals from the view.  We know that
	 * any Vars in the quals must reference the one base relation, so we need
	 * only adjust their varnos to reference the new target (just the same as
	 * we did with the view targetlist).
	 *
	 * For INSERT, the view's quals can be ignored for now.  When we implement
	 * WITH CHECK OPTION, this might be a good place to collect them.
	 */
	if (parsetree->commandType != CMD_INSERT &&
		viewquery->jointree->quals != NULL)
	{
		Node	   *viewqual = (Node *) copyObject(viewquery->jointree->quals);

		ChangeVarNodes(viewqual, base_rt_index, new_rt_index, 0);
		AddQual(parsetree, (Node *) viewqual);
	}

	return parsetree;
}


/*
 * RewriteQuery -
 *	  rewrites the query and apply the rules again on the queries rewritten
 *
 * rewrite_events is a list of open query-rewrite actions, so we can detect
 * infinite recursion.
 */
static List *
RewriteQuery(Query *parsetree, List *rewrite_events)
{
	CmdType		event = parsetree->commandType;
	bool		instead = false;
	bool		returning = false;
	Query	   *qual_product = NULL;
	List	   *rewritten = NIL;
	ListCell   *lc1;

	/*
	 * First, recursively process any insert/update/delete statements in WITH
	 * clauses.  (We have to do this first because the WITH clauses may get
	 * copied into rule actions below.)
	 */
	foreach(lc1, parsetree->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc1);
		Query	   *ctequery = (Query *) cte->ctequery;
		List	   *newstuff;

		Assert(IsA(ctequery, Query));

		if (ctequery->commandType == CMD_SELECT)
			continue;

		newstuff = RewriteQuery(ctequery, rewrite_events);

		/*
		 * Currently we can only handle unconditional, single-statement DO
		 * INSTEAD rules correctly; we have to get exactly one Query out of
		 * the rewrite operation to stuff back into the CTE node.
		 */
		if (list_length(newstuff) == 1)
		{
			/* Push the single Query back into the CTE node */
			ctequery = (Query *) linitial(newstuff);
			Assert(IsA(ctequery, Query));
			/* WITH queries should never be canSetTag */
			Assert(!ctequery->canSetTag);
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
	 * If the statement is an insert, update, or delete, adjust its targetlist
	 * as needed, and then fire INSERT/UPDATE/DELETE rules on it.
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
		List	   *product_queries;

		result_relation = parsetree->resultRelation;
		Assert(result_relation != 0);
		rt_entry = rt_fetch(result_relation, parsetree->rtable);
		Assert(rt_entry->rtekind == RTE_RELATION);

		/*
		 * We can use NoLock here since either the parser or
		 * AcquireRewriteLocks should have locked the rel already.
		 */
		rt_entry_relation = heap_open(rt_entry->relid, NoLock);

		/*
		 * Rewrite the targetlist as needed for the command type.
		 */
		if (event == CMD_INSERT)
		{
			RangeTblEntry *values_rte = NULL;

			/*
			 * If it's an INSERT ... VALUES (...), (...), ... there will be a
			 * single RTE for the VALUES targetlists.
			 */
			if (list_length(parsetree->jointree->fromlist) == 1)
			{
				RangeTblRef *rtr = (RangeTblRef *) linitial(parsetree->jointree->fromlist);

				if (IsA(rtr, RangeTblRef))
				{
					RangeTblEntry *rte = rt_fetch(rtr->rtindex,
												  parsetree->rtable);

					if (rte->rtekind == RTE_VALUES)
						values_rte = rte;
				}
			}

			if (values_rte)
			{
				List	   *attrnos;

				/* Process the main targetlist ... */
				rewriteTargetListIU(parsetree, rt_entry_relation, &attrnos);
				/* ... and the VALUES expression lists */
				rewriteValuesRTE(values_rte, rt_entry_relation, attrnos);
			}
			else
			{
				/* Process just the main targetlist */
				rewriteTargetListIU(parsetree, rt_entry_relation, NULL);
			}
		}
		else if (event == CMD_UPDATE)
		{
			rewriteTargetListIU(parsetree, rt_entry_relation, NULL);
			rewriteTargetListUD(parsetree, rt_entry, rt_entry_relation);
		}
		else if (event == CMD_DELETE)
		{
			rewriteTargetListUD(parsetree, rt_entry, rt_entry_relation);
		}
		else
			elog(ERROR, "unrecognized commandType: %d", (int) event);

		/*
		 * Collect and apply the appropriate rules.
		 */
		locks = matchLocks(event, rt_entry_relation->rd_rules,
						   result_relation, parsetree);

		product_queries = fireRules(parsetree,
									result_relation,
									event,
									locks,
									&instead,
									&returning,
									&qual_product);

		/*
		 * If there were no INSTEAD rules, and the target relation is a view
		 * without any INSTEAD OF triggers, see if the view can be
		 * automatically updated.  If so, we perform the necessary query
		 * transformation here and add the resulting query to the
		 * product_queries list, so that it gets recursively rewritten if
		 * necessary.
		 */
		if (!instead && qual_product == NULL &&
			rt_entry_relation->rd_rel->relkind == RELKIND_VIEW &&
			!view_has_instead_trigger(rt_entry_relation, event))
		{
			/*
			 * This throws an error if the view can't be automatically
			 * updated, but that's OK since the query would fail at runtime
			 * anyway.
			 */
			parsetree = rewriteTargetView(parsetree, rt_entry_relation);

			/*
			 * At this point product_queries contains any DO ALSO rule
			 * actions. Add the rewritten query before or after those.	This
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
			rewrite_events = lcons(rev, rewrite_events);

			foreach(n, product_queries)
			{
				Query	   *pt = (Query *) lfirst(n);
				List	   *newstuff;

				newstuff = RewriteQuery(pt, rewrite_events);
				rewritten = list_concat(rewritten, newstuff);
			}

			rewrite_events = list_delete_first(rewrite_events);
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

		heap_close(rt_entry_relation, NoLock);
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
	uint32		input_query_id = parsetree->queryId;
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
	querylist = RewriteQuery(parsetree, NIL);

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

		query = fireRIRrules(query, NIL, false);

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
	 * allowed to set the tag.	(Note these rules can leave us with no query
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
