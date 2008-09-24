/*-------------------------------------------------------------------------
 *
 * rewriteHandler.c
 *		Primary module of query rewriter.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/rewrite/rewriteHandler.c,v 1.168.2.2 2008/09/24 16:53:00 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "parser/analyze.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


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
static void rewriteTargetList(Query *parsetree, Relation target_relation,
				  List **attrno_list);
static TargetEntry *process_matched_tle(TargetEntry *src_tle,
					TargetEntry *prior_tle,
					const char *attrName);
static Node *get_assignment_input(Node *node);
static void rewriteValuesRTE(RangeTblEntry *rte, Relation target_relation,
				 List *attrnos);
static void markQueryForLocking(Query *qry, Node *jtnode,
								bool forUpdate, bool noWait);
static List *matchLocks(CmdType event, RuleLock *rulelocks,
		   int varno, Query *parsetree);
static Query *fireRIRrules(Query *parsetree, List *activeRIRs);


/*
 * AcquireRewriteLocks -
 *	  Acquire suitable locks on all the relations mentioned in the Query.
 *	  These locks will ensure that the relation schemas don't change under us
 *	  while we are rewriting and planning the query.
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
 * prevent the column drop.)  To support get_rte_attribute_is_dropped(),
 * we replace join alias vars that reference dropped columns with NULL Const
 * nodes.
 *
 * (In PostgreSQL 8.0, we did not do this processing but instead had
 * get_rte_attribute_is_dropped() recurse to detect dropped columns in joins.
 * That approach had horrible performance unfortunately; in particular
 * construction of a nested join was O(N^2) in the nesting depth.)
 */
void
AcquireRewriteLocks(Query *parsetree)
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
				 * relation is accessed FOR UPDATE/SHARE or not.  We can't
				 * just grab AccessShareLock because then the executor would
				 * be trying to upgrade the lock, leading to possible
				 * deadlocks.
				 */
				if (rt_index == parsetree->resultRelation)
					lockmode = RowExclusiveLock;
				else if (get_rowmark(parsetree, rt_index))
					lockmode = RowShareLock;
				else
					lockmode = AccessShareLock;

				rel = heap_open(rte->relid, lockmode);
				heap_close(rel, NoLock);
				break;

			case RTE_JOIN:

				/*
				 * Scan the join's alias var list to see if any columns have
				 * been dropped, and if so replace those Vars with NULL
				 * Consts.
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
					Var		   *aliasvar = (Var *) lfirst(ll);

					/*
					 * If the list item isn't a simple Var, then it must
					 * represent a merged column, ie a USING column, and so it
					 * couldn't possibly be dropped, since it's referenced in
					 * the join clause.  (Conceivably it could also be a NULL
					 * constant already?  But that's OK too.)
					 */
					if (IsA(aliasvar, Var))
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
							/*
							 * can't use vartype here, since that might be a
							 * now-dropped type OID, but it doesn't really
							 * matter what type the Const claims to be.
							 */
							aliasvar = (Var *) makeNullConst(INT4OID);
						}
					}
					newaliasvars = lappend(newaliasvars, aliasvar);
				}
				rte->joinaliasvars = newaliasvars;
				break;

			case RTE_SUBQUERY:

				/*
				 * The subquery RTE itself is all right, but we have to
				 * recurse to process the represented subquery.
				 */
				AcquireRewriteLocks(rte->subquery);
				break;

			default:
				/* ignore other types of RTEs */
				break;
		}
	}

	/*
	 * Recurse into sublink subqueries, too.  But we already did the ones in
	 * the rtable.
	 */
	if (parsetree->hasSubLinks)
		query_tree_walker(parsetree, acquireLocksOnSubLinks, NULL,
						  QTW_IGNORE_RT_SUBQUERIES);
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
		AcquireRewriteLocks((Query *) sub->subselect);
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
	AcquireRewriteLocks(rule_action);
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
	/* but references to *OLD* should point at original rt_index */
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
				break;		/* no need to keep scanning rtable */
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
	 * Event Qualification forces copying of parsetree and splitting into two
	 * queries one w/rule_qual, one w/NOT rule_qual. Also add user query qual
	 * onto rule action
	 */
	AddQual(sub_action, rule_qual);

	AddQual(sub_action, parsetree->jointree->quals);

	/*
	 * Rewrite new.attribute w/ right hand side of target-list entry for
	 * appropriate field name in insert/update.
	 *
	 * KLUGE ALERT: since ResolveNew returns a mutated copy, we can't just
	 * apply it to sub_action; we have to remember to update the sublink
	 * inside rule_action, too.
	 */
	if ((event == CMD_INSERT || event == CMD_UPDATE) &&
		sub_action->commandType != CMD_UTILITY)
	{
		sub_action = (Query *) ResolveNew((Node *) sub_action,
										  new_varno,
										  0,
										  rt_fetch(new_varno,
												   sub_action->rtable),
										  parsetree->targetList,
										  event,
										  current_varno);
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
			ResolveNew((Node *) parsetree->returningList,
					   parsetree->resultRelation,
					   0,
					   rt_fetch(parsetree->resultRelation,
								parsetree->rtable),
					   rule_action->returningList,
					   CMD_SELECT,
					   0);

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
 * rewriteTargetList - rewrite INSERT/UPDATE targetlist into standard form
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
 *		foo = array_set(array_set(foo, 2, 42), 4, 43)
 *
 * 3. Sort the tlist into standard order: non-junk fields in order by resno,
 * then junk fields (these in no particular order).
 *
 * We must do items 1 and 2 before firing rewrite rules, else rewritten
 * references to NEW.foo will produce wrong or incomplete results.	Item 3
 * is not needed for rewriting, but will be needed by the planner, and we
 * can do it essentially for free while handling items 1 and 2.
 *
 * If attrno_list isn't NULL, we return an additional output besides the
 * rewritten targetlist: an integer list of the assigned-to attnums, in
 * order of the original tlist's non-junk entries.  This is needed for
 * processing VALUES RTEs.
 */
static void
rewriteTargetList(Query *parsetree, Relation target_relation,
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
												  att_tup->attlen,
												  (Datum) 0,
												  true, /* isnull */
												  att_tup->attbyval);
					/* this is to catch a NOT NULL domain constraint */
					new_expr = coerce_to_domain(new_expr,
												InvalidOid, -1,
												att_tup->atttypid,
												COERCE_IMPLICIT_CAST,
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
		elog(ERROR, "can't happen");
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
								 COERCE_IMPLICIT_CAST);
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
 * the appropriate default expressions.  The other aspects of rewriteTargetList
 * need be applied only to the query's targetlist proper.
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
												  att_tup->attlen,
												  (Datum) 0,
												  true, /* isnull */
												  att_tup->attbyval);
					/* this is to catch a NOT NULL domain constraint */
					new_expr = coerce_to_domain(new_expr,
												InvalidOid, -1,
												att_tup->atttypid,
												COERCE_IMPLICIT_CAST,
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
				  List *activeRIRs)
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

	/*
	 * Make a modifiable copy of the view query, and acquire needed locks on
	 * the relations it mentions.
	 */
	rule_action = copyObject(linitial(rule->actions));

	AcquireRewriteLocks(rule_action);

	/*
	 * Recursively expand any view references inside the view.
	 */
	rule_action = fireRIRrules(rule_action, activeRIRs);

	/*
	 * VIEWs are really easy --- just plug the view query in as a subselect,
	 * replacing the relation's original RTE.
	 */
	rte = rt_fetch(rt_index, parsetree->rtable);

	rte->rtekind = RTE_SUBQUERY;
	rte->relid = InvalidOid;
	rte->subquery = rule_action;
	rte->inh = false;			/* must not be set for a subquery */

	/*
	 * We move the view's permission check data down to its rangetable. The
	 * checks will actually be done against the *OLD* entry therein.
	 */
	subrte = rt_fetch(PRS2_OLD_VARNO, rule_action->rtable);
	Assert(subrte->relid == relation->rd_id);
	subrte->requiredPerms = rte->requiredPerms;
	subrte->checkAsUser = rte->checkAsUser;

	rte->requiredPerms = 0;		/* no permission check on subquery itself */
	rte->checkAsUser = InvalidOid;

	/*
	 * FOR UPDATE/SHARE of view?
	 */
	if ((rc = get_rowmark(parsetree, rt_index)) != NULL)
	{
		/*
		 * Remove the view from the list of rels that will actually be marked
		 * FOR UPDATE/SHARE by the executor.  It will still be access-checked
		 * for write access, though.
		 */
		parsetree->rowMarks = list_delete_ptr(parsetree->rowMarks, rc);

		/*
		 * Set up the view's referenced tables as if FOR UPDATE/SHARE.
		 */
		markQueryForLocking(rule_action, (Node *) rule_action->jointree,
							rc->forUpdate, rc->noWait);
	}

	return parsetree;
}

/*
 * Recursively mark all relations used by a view as FOR UPDATE/SHARE.
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
markQueryForLocking(Query *qry, Node *jtnode, bool forUpdate, bool noWait)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		int			rti = ((RangeTblRef *) jtnode)->rtindex;
		RangeTblEntry *rte = rt_fetch(rti, qry->rtable);

		if (rte->rtekind == RTE_RELATION)
		{
			applyLockingClause(qry, rti, forUpdate, noWait);
			rte->requiredPerms |= ACL_SELECT_FOR_UPDATE;
		}
		else if (rte->rtekind == RTE_SUBQUERY)
		{
			/* FOR UPDATE/SHARE of subquery is propagated to subquery's rels */
			markQueryForLocking(rte->subquery, (Node *) rte->subquery->jointree,
								forUpdate, noWait);
		}
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
			markQueryForLocking(qry, lfirst(l), forUpdate, noWait);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		markQueryForLocking(qry, j->larg, forUpdate, noWait);
		markQueryForLocking(qry, j->rarg, forUpdate, noWait);
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
											   activeRIRs);
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
fireRIRrules(Query *parsetree, List *activeRIRs)
{
	int			rt_index;

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
			continue;
		}

		/*
		 * Joins and other non-relation RTEs can be ignored completely.
		 */
		if (rte->rtekind != RTE_RELATION)
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
											  activeRIRs);
			}

			activeRIRs = list_delete_first(activeRIRs);
		}

		heap_close(rel, NoLock);
	}

	/*
	 * Recurse into sublink subqueries, too.  But we already did the ones in
	 * the rtable.
	 */
	if (parsetree->hasSubLinks)
		query_tree_walker(parsetree, fireRIRonSubLink, (void *) activeRIRs,
						  QTW_IGNORE_RT_SUBQUERIES);

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
		new_qual = ResolveNew(new_qual,
							  PRS2_NEW_VARNO,
							  0,
							  rt_fetch(rt_index, parsetree->rtable),
							  parsetree->targetList,
							  event,
							  rt_index);
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

	/*
	 * If the statement is an update, insert or delete - fire rules on it.
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
		 * If it's an INSERT or UPDATE, rewrite the targetlist into standard
		 * form.  This will be needed by the planner anyway, and doing it now
		 * ensures that any references to NEW.field will behave sanely.
		 */
		if (event == CMD_UPDATE)
			rewriteTargetList(parsetree, rt_entry_relation, NULL);
		else if (event == CMD_INSERT)
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
				rewriteTargetList(parsetree, rt_entry_relation, &attrnos);
				/* ... and the VALUES expression lists */
				rewriteValuesRTE(values_rte, rt_entry_relation, attrnos);
			}
			else
			{
				/* Process just the main targetlist */
				rewriteTargetList(parsetree, rt_entry_relation, NULL);
			}
		}

		/*
		 * Collect and apply the appropriate rules.
		 */
		locks = matchLocks(event, rt_entry_relation->rd_rules,
						   result_relation, parsetree);

		if (locks != NIL)
		{
			List	   *product_queries;

			product_queries = fireRules(parsetree,
										result_relation,
										event,
										locks,
										&instead,
										&returning,
										&qual_product);

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
	List	   *querylist;
	List	   *results = NIL;
	ListCell   *l;
	CmdType		origCmdType;
	bool		foundOriginalQuery;
	Query	   *lastInstead;

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
	 */
	foreach(l, querylist)
	{
		Query	   *query = (Query *) lfirst(l);

		query = fireRIRrules(query, NIL);

		/*
		 * If the query target was rewritten as a view, complain.
		 */
		if (query->resultRelation)
		{
			RangeTblEntry *rte = rt_fetch(query->resultRelation,
										  query->rtable);

			if (rte->rtekind == RTE_SUBQUERY)
			{
				switch (query->commandType)
				{
					case CMD_INSERT:
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot insert into a view"),
								 errhint("You need an unconditional ON INSERT DO INSTEAD rule.")));
						break;
					case CMD_UPDATE:
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot update a view"),
								 errhint("You need an unconditional ON UPDATE DO INSTEAD rule.")));
						break;
					case CMD_DELETE:
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot delete from a view"),
								 errhint("You need an unconditional ON DELETE DO INSTEAD rule.")));
						break;
					default:
						elog(ERROR, "unrecognized commandType: %d",
							 (int) query->commandType);
						break;
				}
			}
		}

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
