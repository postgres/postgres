/*-------------------------------------------------------------------------
 *
 * rewriteHandler.c
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteHandler.c,v 1.85 2000/12/06 23:55:18 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/prep.h"
#include "optimizer/var.h"
#include "parser/analyze.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parse_target.h"
#include "parser/parsetree.h"
#include "parser/parse_type.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"


static RewriteInfo *gatherRewriteMeta(Query *parsetree,
				  Query *rule_action,
				  Node *rule_qual,
				  int rt_index,
				  CmdType event,
				  bool instead_flag);
static List *adjustJoinTreeList(Query *parsetree, int rt_index, bool *found);
static List *matchLocks(CmdType event, RuleLock *rulelocks,
						int varno, Query *parsetree);
static Query *fireRIRrules(Query *parsetree);


/*
 * gatherRewriteMeta -
 *	  Gather meta information about parsetree, and rule. Fix rule body
 *	  and qualifier so that they can be mixed with the parsetree and
 *	  maintain semantic validity
 */
static RewriteInfo *
gatherRewriteMeta(Query *parsetree,
				  Query *rule_action,
				  Node *rule_qual,
				  int rt_index,
				  CmdType event,
				  bool instead_flag)
{
	RewriteInfo *info;
	Query	   *sub_action;
	Query	  **sub_action_ptr;
	int			rt_length;

	info = (RewriteInfo *) palloc(sizeof(RewriteInfo));
	info->rt_index = rt_index;
	info->event = event;
	info->instead_flag = instead_flag;
	info->rule_action = (Query *) copyObject(rule_action);
	info->rule_qual = (Node *) copyObject(rule_qual);
	if (info->rule_action == NULL)
	{
		info->nothing = TRUE;
		return info;
	}
	info->nothing = FALSE;
	info->action = info->rule_action->commandType;
	info->current_varno = rt_index;
	rt_length = length(parsetree->rtable);
	info->new_varno = PRS2_NEW_VARNO + rt_length;

	/*
	 * Adjust rule action and qual to offset its varnos, so that we can
	 * merge its rtable into the main parsetree's rtable.
	 *
	 * If the rule action is an INSERT...SELECT, the OLD/NEW rtable
	 * entries will be in the SELECT part, and we have to modify that
	 * rather than the top-level INSERT (kluge!).
	 */
	sub_action = getInsertSelectQuery(info->rule_action, &sub_action_ptr);

	OffsetVarNodes((Node *) sub_action, rt_length, 0);
	OffsetVarNodes(info->rule_qual, rt_length, 0);
	/* but references to *OLD* should point at original rt_index */
	ChangeVarNodes((Node *) sub_action,
				   PRS2_OLD_VARNO + rt_length, rt_index, 0);
	ChangeVarNodes(info->rule_qual,
				   PRS2_OLD_VARNO + rt_length, rt_index, 0);

	/*
	 * We want the main parsetree's rtable to end up as the concatenation
	 * of its original contents plus those of all the relevant rule
	 * actions.  Also store same into all the rule_action rtables.
	 * Some of the entries may be unused after we finish rewriting, but
	 * if we tried to clean those out we'd have a much harder job to
	 * adjust RT indexes in the query's Vars.  It's OK to have unused
	 * RT entries, since planner will ignore them.
	 *
	 * NOTE KLUGY HACK: we assume the parsetree rtable had at least one
	 * entry to begin with (OK enough, else where'd the rule come from?).
	 * Because of this, if multiple rules nconc() their rtable additions
	 * onto parsetree->rtable, they'll all see the same rtable because
	 * they all have the same list head pointer.
	 */
	parsetree->rtable = nconc(parsetree->rtable,
							  sub_action->rtable);
	sub_action->rtable = parsetree->rtable;

	/*
	 * Each rule action's jointree should be the main parsetree's jointree
	 * plus that rule's jointree, but *without* the original rtindex
	 * that we're replacing (if present, which it won't be for INSERT).
	 * Note that if the rule refers to OLD, its jointree will add back
	 * a reference to rt_index.
	 */
	{
		bool	found;
		List   *newjointree = adjustJoinTreeList(parsetree,
												 rt_index,
												 &found);

		sub_action->jointree->fromlist =
			nconc(newjointree, sub_action->jointree->fromlist);
	}

	/*
	 * We copy the qualifications of the parsetree to the action and vice
	 * versa. So force hasSubLinks if one of them has it. If this is not
	 * right, the flag will get cleared later, but we mustn't risk having
	 * it not set when it needs to be.
	 */
	if (parsetree->hasSubLinks)
		sub_action->hasSubLinks = TRUE;
	else if (sub_action->hasSubLinks)
		parsetree->hasSubLinks = TRUE;

	/*
	 * Event Qualification forces copying of parsetree and
	 * splitting into two queries one w/rule_qual, one w/NOT
	 * rule_qual. Also add user query qual onto rule action
	 */
	AddQual(sub_action, info->rule_qual);

	AddQual(sub_action, parsetree->jointree->quals);

	/*
	 * Rewrite new.attribute w/ right hand side of target-list
	 * entry for appropriate field name in insert/update.
	 *
	 * KLUGE ALERT: since ResolveNew returns a mutated copy, we can't just
	 * apply it to sub_action; we have to remember to update the sublink
	 * inside info->rule_action, too.
	 */
	if (info->event == CMD_INSERT || info->event == CMD_UPDATE)
	{
		sub_action = (Query *) ResolveNew((Node *) sub_action,
										  info->new_varno,
										  0,
										  parsetree->targetList,
										  info->event,
										  info->current_varno);
		if (sub_action_ptr)
			*sub_action_ptr = sub_action;
		else
			info->rule_action = sub_action;
	}

	return info;
}

/*
 * Copy the query's jointree list, and attempt to remove any occurrence
 * of the given rt_index as a top-level join item (we do not look for it
 * within join items; this is OK because we are only expecting to find it
 * as an UPDATE or DELETE target relation, which will be at the top level
 * of the join).  Returns modified jointree list --- original list
 * is not changed.  *found is set to indicate if we found the rt_index.
 */
static List *
adjustJoinTreeList(Query *parsetree, int rt_index, bool *found)
{
	List	   *newjointree = listCopy(parsetree->jointree->fromlist);
	List	   *jjt;

	*found = false;
	foreach(jjt, newjointree)
	{
		RangeTblRef *rtr = lfirst(jjt);

		if (IsA(rtr, RangeTblRef) && rtr->rtindex == rt_index)
		{
			newjointree = lremove(rtr, newjointree);
			*found = true;
			break;
		}
	}
	return newjointree;
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
	List	   *real_locks = NIL;
	int			nlocks;
	int			i;

	Assert(rulelocks != NULL);	/* we get called iff there is some lock */
	Assert(parsetree != NULL);

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
				real_locks = lappend(real_locks, oneLock);
		}
	}

	return real_locks;
}


static Query *
ApplyRetrieveRule(Query *parsetree,
				  RewriteRule *rule,
				  int rt_index,
				  bool relation_level,
				  Relation relation,
				  bool relIsUsed)
{
	Query	   *rule_action;
	RangeTblEntry *rte,
			   *subrte;
	List	   *l;

	if (length(rule->actions) != 1)
		elog(ERROR, "ApplyRetrieveRule: expected just one rule action");
	if (rule->qual != NULL)
		elog(ERROR, "ApplyRetrieveRule: can't handle qualified ON SELECT rule");
	if (! relation_level)
		elog(ERROR, "ApplyRetrieveRule: can't handle per-attribute ON SELECT rule");

	/*
	 * Make a modifiable copy of the view query, and recursively expand
	 * any view references inside it.
	 */
	rule_action = copyObject(lfirst(rule->actions));

	rule_action = fireRIRrules(rule_action);

	/*
	 * VIEWs are really easy --- just plug the view query in as a subselect,
	 * replacing the relation's original RTE.
	 */
	rte = rt_fetch(rt_index, parsetree->rtable);

	rte->relname = NULL;
	rte->relid = InvalidOid;
	rte->subquery = rule_action;
	rte->inh = false;			/* must not be set for a subquery */

	/*
	 * We move the view's permission check data down to its rangetable.
	 * The checks will actually be done against the *OLD* entry therein.
	 */
	subrte = rt_fetch(PRS2_OLD_VARNO, rule_action->rtable);
	Assert(subrte->relid == relation->rd_id);
	subrte->checkForRead = rte->checkForRead;
	subrte->checkForWrite = rte->checkForWrite;

	rte->checkForRead = false;	/* no permission check on subquery itself */
	rte->checkForWrite = false;

	/*
	 * FOR UPDATE of view?
	 */
	if (intMember(rt_index, parsetree->rowMarks))
	{
		Index		innerrti = 1;

		/*
		 * Remove the view from the list of rels that will actually be
		 * marked FOR UPDATE by the executor.  It will still be access-
		 * checked for write access, though.
		 */
		parsetree->rowMarks = lremovei(rt_index, parsetree->rowMarks);

		/*
		 * Set up the view's referenced tables as if FOR UPDATE.
		 */
		foreach(l, rule_action->rtable)
		{
			subrte = (RangeTblEntry *) lfirst(l);

			/*
			 * RTable of VIEW has two entries of VIEW itself - skip them!
			 * Also keep hands off of sub-subqueries.
			 */
			if (innerrti != PRS2_OLD_VARNO && innerrti != PRS2_NEW_VARNO &&
				subrte->relid != InvalidOid)
			{
				if (!intMember(innerrti, rule_action->rowMarks))
					rule_action->rowMarks = lappendi(rule_action->rowMarks,
													 innerrti);
				subrte->checkForWrite = true;
			}
			innerrti++;
		}
	}

	return parsetree;
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
fireRIRonSubLink(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
	{
		SubLink    *sub = (SubLink *) node;

		/* Do what we came for */
		sub->subselect = (Node *) fireRIRrules((Query *) (sub->subselect));
		/* Fall through to process lefthand args of SubLink */
	}
	/*
	 * Do NOT recurse into Query nodes, because fireRIRrules already
	 * processed subselects of subselects for us.
	 */
	return expression_tree_walker(node, fireRIRonSubLink,
								  (void *) context);
}


/*
 * fireRIRrules -
 *	Apply all RIR rules on each rangetable entry in a query
 */
static Query *
fireRIRrules(Query *parsetree)
{
	int			rt_index;

	/*
	 * don't try to convert this into a foreach loop, because rtable list
	 * can get changed each time through...
	 */
	rt_index = 0;
	while (rt_index < length(parsetree->rtable))
	{
		RangeTblEntry *rte;
		Relation	rel;
		List	   *locks;
		RuleLock   *rules;
		RewriteRule *rule;
		LOCKMODE	lockmode;
		bool		relIsUsed;
		int			i;
		List	   *l;

		++rt_index;

		rte = rt_fetch(rt_index, parsetree->rtable);

		/*
		 * A subquery RTE can't have associated rules, so there's nothing
		 * to do to this level of the query, but we must recurse into the
		 * subquery to expand any rule references in it.
		 */
		if (rte->subquery)
		{
			rte->subquery = fireRIRrules(rte->subquery);
			continue;
		}

		/*
		 * If the table is not referenced in the query, then we ignore it.
		 * This prevents infinite expansion loop due to new rtable entries
		 * inserted by expansion of a rule. A table is referenced if it is
		 * part of the join set (a source table), or is referenced by any
		 * Var nodes, or is the result table.
		 */
		relIsUsed = rangeTableEntry_used((Node *) parsetree, rt_index, 0);

		if (!relIsUsed && rt_index != parsetree->resultRelation)
			continue;

		/*
		 * This may well be the first access to the relation during
		 * the current statement (it will be, if this Query was extracted
		 * from a rule or somehow got here other than via the parser).
		 * Therefore, grab the appropriate lock type for the relation,
		 * and do not release it until end of transaction.  This protects
		 * the rewriter and planner against schema changes mid-query.
		 *
		 * If the relation is the query's result relation, then RewriteQuery()
		 * already got the right lock on it, so we need no additional lock.
		 * Otherwise, check to see if the relation is accessed FOR UPDATE
		 * or not.
		 */
		if (rt_index == parsetree->resultRelation)
			lockmode = NoLock;
		else if (intMember(rt_index, parsetree->rowMarks))
			lockmode = RowShareLock;
		else
			lockmode = AccessShareLock;

		rel = heap_openr(rte->relname, lockmode);

		rules = rel->rd_rules;
		if (rules == NULL)
		{
			heap_close(rel, NoLock);
			continue;
		}

		/*
		 * Collect the RIR rules that we must apply
		 */
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
		 * Now apply them
		 */
		foreach(l, locks)
		{
			rule = lfirst(l);

			parsetree = ApplyRetrieveRule(parsetree,
										  rule,
										  rt_index,
										  rule->attrno == -1,
										  rel,
										  relIsUsed);
		}

		heap_close(rel, NoLock);
	}

	/*
	 * Recurse into sublink subqueries, too.
	 */
	if (parsetree->hasSubLinks)
		query_tree_walker(parsetree, fireRIRonSubLink, NULL,
						  false /* already handled the ones in rtable */);

	/*
	 * If the query was marked having aggregates, check if this is
	 * still true after rewriting.	Ditto for sublinks.  Note there
	 * should be no aggs in the qual at this point.  (Does this code
	 * still do anything useful?  The view-becomes-subselect-in-FROM
	 * approach doesn't look like it could remove aggs or sublinks...)
	 */
	if (parsetree->hasAggs)
	{
		parsetree->hasAggs = checkExprHasAggs((Node *) parsetree);
		if (parsetree->hasAggs)
			if (checkExprHasAggs((Node *) parsetree->jointree))
				elog(ERROR, "fireRIRrules: failed to remove aggs from qual");
	}
	if (parsetree->hasSubLinks)
	{
		parsetree->hasSubLinks = checkExprHasSubLink((Node *) parsetree);
	}

	return parsetree;
}


/*
 * idea is to fire regular rules first, then qualified instead
 * rules and unqualified instead rules last. Any lemming is counted for.
 */
static List *
orderRules(List *locks)
{
	List	   *regular = NIL;
	List	   *instead_rules = NIL;
	List	   *instead_qualified = NIL;
	List	   *i;

	foreach(i, locks)
	{
		RewriteRule *rule_lock = (RewriteRule *) lfirst(i);

		if (rule_lock->isInstead)
		{
			if (rule_lock->qual == NULL)
				instead_rules = lappend(instead_rules, rule_lock);
			else
				instead_qualified = lappend(instead_qualified, rule_lock);
		}
		else
			regular = lappend(regular, rule_lock);
	}
	return nconc(nconc(regular, instead_qualified), instead_rules);
}


/*
 * Modify the given query by adding 'AND NOT rule_qual' to its qualification.
 * This is used to generate suitable "else clauses" for conditional INSTEAD
 * rules.
 *
 * The rule_qual may contain references to OLD or NEW.  OLD references are
 * replaced by references to the specified rt_index (the relation that the
 * rule applies to).  NEW references are only possible for INSERT and UPDATE
 * queries on the relation itself, and so they should be replaced by copies
 * of the related entries in the query's own targetlist.
 */
static Query *
CopyAndAddQual(Query *parsetree,
			   Node *rule_qual,
			   int rt_index,
			   CmdType event)
{
	Query	   *new_tree = (Query *) copyObject(parsetree);
	Node	   *new_qual = (Node *) copyObject(rule_qual);

	/* Fix references to OLD */
	ChangeVarNodes(new_qual, PRS2_OLD_VARNO, rt_index, 0);
	/* Fix references to NEW */
	if (event == CMD_INSERT || event == CMD_UPDATE)
		new_qual = ResolveNew(new_qual,
							  PRS2_NEW_VARNO,
							  0,
							  parsetree->targetList,
							  event,
							  rt_index);
	/* And attach the fixed qual */
	AddNotQual(new_tree, new_qual);

	return new_tree;
}



/*
 *	fireRules -
 *	   Iterate through rule locks applying rules.
 *	   All rules create their own parsetrees. Instead rules
 *	   with rule qualification save the original parsetree
 *	   and add their negated qualification to it. Real instead
 *	   rules finally throw away the original parsetree.
 *
 *	   remember: reality is for dead birds -- glass
 *
 */
static List *
fireRules(Query *parsetree,
		  int rt_index,
		  CmdType event,
		  bool *instead_flag,
		  List *locks,
		  List **qual_products)
{
	List	   *results = NIL;
	List	   *i;

	/* choose rule to fire from list of rules */
	if (locks == NIL)
		return NIL;

	locks = orderRules(locks);	/* real instead rules last */

	foreach(i, locks)
	{
		RewriteRule *rule_lock = (RewriteRule *) lfirst(i);
		Node	   *event_qual;
		List	   *actions;
		List	   *r;

		/* multiple rule action time */
		*instead_flag = rule_lock->isInstead;
		event_qual = rule_lock->qual;
		actions = rule_lock->actions;

		if (event_qual != NULL && *instead_flag)
		{
			Query	   *qual_product;

			/* ----------
			 * If there are instead rules with qualifications,
			 * the original query is still performed. But all
			 * the negated rule qualifications of the instead
			 * rules are added so it does its actions only
			 * in cases where the rule quals of all instead
			 * rules are false. Think of it as the default
			 * action in a case. We save this in *qual_products
			 * so deepRewriteQuery() can add it to the query
			 * list after we mangled it up enough.
			 * ----------
			 */
			if (*qual_products == NIL)
				qual_product = parsetree;
			else
				qual_product = (Query *) lfirst(*qual_products);

			qual_product = CopyAndAddQual(qual_product,
										  event_qual,
										  rt_index,
										  event);

			*qual_products = makeList1(qual_product);
		}

		foreach(r, actions)
		{
			Query	   *rule_action = lfirst(r);
			RewriteInfo *info;

			if (rule_action->commandType == CMD_NOTHING)
				continue;

			info = gatherRewriteMeta(parsetree, rule_action, event_qual,
									 rt_index, event, *instead_flag);

			/* handle escapable cases, or those handled by other code */
			if (info->nothing)
			{
				if (*instead_flag)
					return NIL;
				else
					continue;
			}

			results = lappend(results, info->rule_action);

			pfree(info);
		}

		/* ----------
		 * If this was an unqualified instead rule,
		 * throw away an eventually saved 'default' parsetree
		 * ----------
		 */
		if (event_qual == NULL && *instead_flag)
			*qual_products = NIL;
	}
	return results;
}



static List *
RewriteQuery(Query *parsetree, bool *instead_flag, List **qual_products)
{
	CmdType		event;
	List	   *product_queries = NIL;
	int			result_relation;
	RangeTblEntry *rt_entry;
	Relation	rt_entry_relation;
	RuleLock   *rt_entry_locks;

	Assert(parsetree != NULL);

	event = parsetree->commandType;

	/*
	 * SELECT rules are handled later when we have all the queries that
	 * should get executed
	 */
	if (event == CMD_SELECT)
		return NIL;

	/*
	 * Utilities aren't rewritten at all - why is this here?
	 */
	if (event == CMD_UTILITY)
		return NIL;

	/*
	 * the statement is an update, insert or delete - fire rules on it.
	 */
	result_relation = parsetree->resultRelation;
	Assert(result_relation != 0);
	rt_entry = rt_fetch(result_relation, parsetree->rtable);

	/*
	 * This may well be the first access to the result relation during
	 * the current statement (it will be, if this Query was extracted
	 * from a rule or somehow got here other than via the parser).
	 * Therefore, grab the appropriate lock type for a result relation,
	 * and do not release it until end of transaction.  This protects the
	 * rewriter and planner against schema changes mid-query.
	 */
	rt_entry_relation = heap_openr(rt_entry->relname, RowExclusiveLock);

	rt_entry_locks = rt_entry_relation->rd_rules;

	if (rt_entry_locks != NULL)
	{
		List	   *locks = matchLocks(event, rt_entry_locks,
									   result_relation, parsetree);

		product_queries = fireRules(parsetree,
									result_relation,
									event,
									instead_flag,
									locks,
									qual_products);
	}

	heap_close(rt_entry_relation, NoLock); /* keep lock! */

	return product_queries;
}


/*
 * to avoid infinite recursion, we restrict the number of times a query
 * can be rewritten. Detecting cycles is left for the reader as an exercise.
 */
#ifndef REWRITE_INVOKE_MAX
#define REWRITE_INVOKE_MAX		10
#endif

static int	numQueryRewriteInvoked = 0;

/*
 * deepRewriteQuery -
 *	  rewrites the query and apply the rules again on the queries rewritten
 */
static List *
deepRewriteQuery(Query *parsetree)
{
	List	   *n;
	List	   *rewritten = NIL;
	List	   *result;
	bool		instead;
	List	   *qual_products = NIL;

	if (++numQueryRewriteInvoked > REWRITE_INVOKE_MAX)
	{
		elog(ERROR, "query rewritten %d times, may contain cycles",
			 numQueryRewriteInvoked - 1);
	}

	instead = FALSE;
	result = RewriteQuery(parsetree, &instead, &qual_products);

	foreach(n, result)
	{
		Query	   *pt = lfirst(n);
		List	   *newstuff;

		newstuff = deepRewriteQuery(pt);
		if (newstuff != NIL)
			rewritten = nconc(rewritten, newstuff);
	}

	/* ----------
	 * qual_products are the original query with the negated
	 * rule qualification of an instead rule
	 * ----------
	 */
	if (qual_products != NIL)
		rewritten = nconc(rewritten, qual_products);

	/* ----------
	 * The original query is appended last (if no "instead" rule)
	 * because update and delete rule actions might not do
	 * anything if they are invoked after the update or
	 * delete is performed. The command counter increment
	 * between the query execution makes the deleted (and
	 * maybe the updated) tuples disappear so the scans
	 * for them in the rule actions cannot find them.
	 * ----------
	 */
	if (!instead)
		rewritten = lappend(rewritten, parsetree);

	return rewritten;
}


/*
 * QueryRewriteOne -
 *	  rewrite one query
 */
static List *
QueryRewriteOne(Query *parsetree)
{
	numQueryRewriteInvoked = 0;

	/*
	 * take a deep breath and apply all the rewrite rules - ay
	 */
	return deepRewriteQuery(parsetree);
}


/*
 * QueryRewrite -
 *	  Primary entry point to the query rewriter.
 *	  Rewrite one query via query rewrite system, possibly returning 0
 *	  or many queries.
 *
 * NOTE: The code in QueryRewrite was formerly in pg_parse_and_plan(), and was
 * moved here so that it would be invoked during EXPLAIN.
 */
List *
QueryRewrite(Query *parsetree)
{
	List	   *querylist;
	List	   *results = NIL;
	List	   *l;

	/*
	 * Step 1
	 *
	 * Apply all non-SELECT rules possibly getting 0 or many queries
	 */
	querylist = QueryRewriteOne(parsetree);

	/*
	 * Step 2
	 *
	 * Apply all the RIR rules on each query
	 */
	foreach(l, querylist)
	{
		Query   *query = (Query *) lfirst(l);

		query = fireRIRrules(query);

		/*
		 * If the query target was rewritten as a view, complain.
		 */
		if (query->resultRelation)
		{
			RangeTblEntry *rte = rt_fetch(query->resultRelation,
										  query->rtable);

			if (rte->subquery)
			{
				switch (query->commandType)
				{
					case CMD_INSERT:
						elog(ERROR, "Cannot insert into a view without an appropriate rule");
						break;
					case CMD_UPDATE:
						elog(ERROR, "Cannot update a view without an appropriate rule");
						break;
					case CMD_DELETE:
						elog(ERROR, "Cannot delete from a view without an appropriate rule");
						break;
					default:
						elog(ERROR, "QueryRewrite: unexpected commandType %d",
							 (int) query->commandType);
						break;
				}
			}
		}

		results = lappend(results, query);
	}

	return results;
}
