/*-------------------------------------------------------------------------
 *
 * rewriteHandler.c
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteHandler.c,v 1.81 2000/09/29 18:21:24 tgl Exp $
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


extern void CheckSelectForUpdate(Query *rule_action);	/* in analyze.c */


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
static Query *Except_Intersect_Rewrite(Query *parsetree);
static void check_targetlists_are_compatible(List *prev_target,
								 List *current_target);
static void create_intersect_list(Node *ptr, List **intersect_list);
static Node *intersect_tree_analyze(Node *tree, Node *first_select,
					   Node *parsetree);

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
	int			rt_length;

	info = (RewriteInfo *) palloc(sizeof(RewriteInfo));
	info->rt_index = rt_index;
	info->event = event;
	info->instead_flag = instead_flag;
	info->rule_action = (Query *) copyObject(rule_action);
	info->rule_qual = (Node *) copyObject(rule_qual);
	if (info->rule_action == NULL)
		info->nothing = TRUE;
	else
	{
		info->nothing = FALSE;
		info->action = info->rule_action->commandType;
		info->current_varno = rt_index;
		rt_length = length(parsetree->rtable);

		/* Adjust rule action and qual to offset its varnos */
		info->new_varno = PRS2_NEW_VARNO + rt_length;
		OffsetVarNodes((Node *) info->rule_action, rt_length, 0);
		OffsetVarNodes(info->rule_qual, rt_length, 0);
		/* but its references to *OLD* should point at original rt_index */
		ChangeVarNodes((Node *) info->rule_action,
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
								  info->rule_action->rtable);
		info->rule_action->rtable = parsetree->rtable;

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

			info->rule_action->jointree->fromlist =
				nconc(newjointree,
					  info->rule_action->jointree->fromlist);
		}

		/*
		 * bug here about replace CURRENT  -- sort of replace current is
		 * deprecated now so this code shouldn't really need to be so
		 * clutzy but.....
		 */
		if (info->action != CMD_SELECT)
		{						/* i.e update XXXXX */
			int			result_reln;
			int			new_result_reln;

			result_reln = info->rule_action->resultRelation;
			switch (result_reln)
			{
				case PRS2_OLD_VARNO:
					new_result_reln = rt_index;
					break;
				case PRS2_NEW_VARNO:	/* XXX */
				default:
					new_result_reln = result_reln + rt_length;
					break;
			}
			info->rule_action->resultRelation = new_result_reln;
		}
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

		CheckSelectForUpdate(rule_action);

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
	RangeTblEntry *rte;
	Relation	rel;
	List	   *locks;
	RuleLock   *rules;
	RewriteRule *rule;
	bool		relIsUsed;
	int			i;
	List	   *l;

	/*
	 * don't try to convert this into a foreach loop, because rtable list
	 * can get changed each time through...
	 */
	rt_index = 0;
	while (rt_index < length(parsetree->rtable))
	{
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

		rel = heap_openr(rte->relname, AccessShareLock);
		rules = rel->rd_rules;
		if (rules == NULL)
		{
			heap_close(rel, AccessShareLock);
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

		heap_close(rel, AccessShareLock);
	}

	/*
	 * Recurse into sublink subqueries, too.
	 */
	if (parsetree->hasSubLinks)
		query_tree_walker(parsetree, fireRIRonSubLink, NULL);

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



static Query *
CopyAndAddQual(Query *parsetree,
			   List *actions,
			   Node *rule_qual,
			   int rt_index,
			   CmdType event)
{
	Query	   *new_tree = (Query *) copyObject(parsetree);
	Node	   *new_qual = NULL;
	Query	   *rule_action = NULL;

	if (actions)
		rule_action = lfirst(actions);
	if (rule_qual != NULL)
		new_qual = (Node *) copyObject(rule_qual);
	if (rule_action != NULL)
	{
		List	   *rtable;
		int			rt_length;
		List	   *jointreelist;

		rtable = new_tree->rtable;
		rt_length = length(rtable);
		rtable = nconc(rtable, copyObject(rule_action->rtable));
		new_tree->rtable = rtable;
		OffsetVarNodes(new_qual, rt_length, 0);
		ChangeVarNodes(new_qual, PRS2_OLD_VARNO + rt_length, rt_index, 0);
		jointreelist = copyObject(rule_action->jointree->fromlist);
		OffsetVarNodes((Node *) jointreelist, rt_length, 0);
		ChangeVarNodes((Node *) jointreelist, PRS2_OLD_VARNO + rt_length,
					   rt_index, 0);
		new_tree->jointree->fromlist = nconc(new_tree->jointree->fromlist,
											 jointreelist);
	}
	/* XXX -- where current doesn't work for instead nothing.... yet */
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
	RewriteInfo *info;
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
			RewriteInfo qual_info;

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

			MemSet(&qual_info, 0, sizeof(qual_info));
			qual_info.event = qual_product->commandType;
			qual_info.current_varno = rt_index;
			qual_info.new_varno = length(qual_product->rtable) + 2;

			qual_product = CopyAndAddQual(qual_product,
										  actions,
										  event_qual,
										  rt_index,
										  event);

			qual_info.rule_action = qual_product;

			if (event == CMD_INSERT || event == CMD_UPDATE)
				FixNew(&qual_info, qual_product);

			*qual_products = makeList1(qual_product);
		}

		foreach(r, actions)
		{
			Query	   *rule_action = lfirst(r);
			Node	   *rule_qual = copyObject(event_qual);

			if (rule_action->commandType == CMD_NOTHING)
				continue;

			/*--------------------------------------------------
			 * We copy the qualifications of the parsetree
			 * to the action and vice versa. So force
			 * hasSubLinks if one of them has it.
			 *
			 * As of 6.4 only parsetree qualifications can
			 * have sublinks. If this changes, we must make
			 * this a node lookup at the end of rewriting.
			 *
			 * Jan
			 *--------------------------------------------------
			 */
			if (parsetree->hasSubLinks && !rule_action->hasSubLinks)
			{
				rule_action = copyObject(rule_action);
				rule_action->hasSubLinks = TRUE;
			}
			if (!parsetree->hasSubLinks && rule_action->hasSubLinks)
				parsetree->hasSubLinks = TRUE;

			/*--------------------------------------------------
			 * Step 1:
			 *	  Rewrite current.attribute or current to tuple variable
			 *	  this appears to be done in parser?
			 *--------------------------------------------------
			 */
			info = gatherRewriteMeta(parsetree, rule_action, rule_qual,
									 rt_index, event, *instead_flag);

			/* handle escapable cases, or those handled by other code */
			if (info->nothing)
			{
				if (*instead_flag)
					return NIL;
				else
					continue;
			}

			if (info->action == info->event &&
				info->event == CMD_SELECT)
				continue;

			/*
			 * Event Qualification forces copying of parsetree and
			 * splitting into two queries one w/rule_qual, one w/NOT
			 * rule_qual. Also add user query qual onto rule action
			 */
			AddQual(info->rule_action, info->rule_qual);

			AddQual(info->rule_action, parsetree->jointree->quals);

			/*--------------------------------------------------
			 * Step 2:
			 *	  Rewrite new.attribute w/ right hand side of target-list
			 *	  entry for appropriate field name in insert/update
			 *--------------------------------------------------
			 */
			if ((info->event == CMD_INSERT) || (info->event == CMD_UPDATE))
				FixNew(info, parsetree);

			/*--------------------------------------------------
			 * Step 3:
			 *	  Simplify? hey, no algorithm for simplification... let
			 *	  the planner do it.
			 *--------------------------------------------------
			 */
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
	rt_entry = rt_fetch(result_relation, parsetree->rtable);
	rt_entry_relation = heap_openr(rt_entry->relname, AccessShareLock);
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

	heap_close(rt_entry_relation, AccessShareLock);

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
 * QueryOneRewrite -
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
 * BasicQueryRewrite -
 *	  rewrite one query via query rewrite system, possibly returning 0
 *	  or many queries
 */
static List *
BasicQueryRewrite(Query *parsetree)
{
	List	   *querylist;
	List	   *results = NIL;
	List	   *l;
	Query	   *query;

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
		query = fireRIRrules((Query *) lfirst(l));
		results = lappend(results, query);
	}

	return results;
}

/*
 * QueryRewrite -
 *	  Primary entry point to the query rewriter.
 *	  Rewrite one query via query rewrite system, possibly returning 0
 *	  or many queries.
 *
 * NOTE: The code in QueryRewrite was formerly in pg_parse_and_plan(), and was
 * moved here so that it would be invoked during EXPLAIN.  The division of
 * labor between this routine and BasicQueryRewrite is not obviously correct
 * ... at least not to me ... tgl 5/99.
 */
List *
QueryRewrite(Query *parsetree)
{
	List	   *rewritten,
			   *rewritten_item;

	/*
	 * Rewrite Union, Intersect and Except Queries to normal Union Queries
	 * using IN and NOT IN subselects
	 */
	if (parsetree->intersectClause)
		parsetree = Except_Intersect_Rewrite(parsetree);

	/* Rewrite basic queries (retrieve, append, delete, replace) */
	rewritten = BasicQueryRewrite(parsetree);

	/*
	 * Rewrite the UNIONS.
	 */
	foreach(rewritten_item, rewritten)
	{
		Query	   *qry = (Query *) lfirst(rewritten_item);
		List	   *union_result = NIL;
		List	   *union_item;

		foreach(union_item, qry->unionClause)
		{
			union_result = nconc(union_result,
						BasicQueryRewrite((Query *) lfirst(union_item)));
		}
		qry->unionClause = union_result;
	}

	return rewritten;
}

/* This function takes two targetlists as arguments and checks if the
 * targetlists are compatible (i.e. both select for the same number of
 * attributes and the types are compatible */
static void
check_targetlists_are_compatible(List *prev_target, List *current_target)
{
	List	   *tl;
	int			prev_len = 0,
				next_len = 0;

	foreach(tl, prev_target)
		if (!((TargetEntry *) lfirst(tl))->resdom->resjunk)
			prev_len++;

	foreach(tl, current_target)
		if (!((TargetEntry *) lfirst(tl))->resdom->resjunk)
			next_len++;

	if (prev_len != next_len)
		elog(ERROR, "Each UNION | EXCEPT | INTERSECT query must have the same number of columns.");

	foreach(tl, current_target)
	{
		TargetEntry	   *next_tle = (TargetEntry *) lfirst(tl);
		TargetEntry	   *prev_tle;
		Oid				itype;
		Oid				otype;

		if (next_tle->resdom->resjunk)
			continue;

		/* This loop must find an entry, since we counted them above. */
		do
		{
			prev_tle = (TargetEntry *) lfirst(prev_target);
			prev_target = lnext(prev_target);
		} while (prev_tle->resdom->resjunk);

		itype = next_tle->resdom->restype;
		otype = prev_tle->resdom->restype;

		/* one or both is a NULL column? then don't convert... */
		if (otype == InvalidOid)
		{
			/* propagate a known type forward, if available */
			if (itype != InvalidOid)
				prev_tle->resdom->restype = itype;
#ifdef NOT_USED
			else
			{
				prev_tle->resdom->restype = UNKNOWNOID;
				next_tle->resdom->restype = UNKNOWNOID;
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

			expr = next_tle->expr;
			expr = CoerceTargetExpr(NULL, expr, itype, otype, -1);
			if (expr == NULL)
			{
				elog(ERROR, "Unable to transform %s to %s"
					 "\n\tEach UNION | EXCEPT | INTERSECT clause must have compatible target types",
					 typeidTypeName(itype),
					 typeidTypeName(otype));
			}
			next_tle->expr = expr;
			next_tle->resdom->restype = otype;
		}

		/* both are UNKNOWN? then evaluate as text... */
		else if (itype == UNKNOWNOID)
		{
			next_tle->resdom->restype = TEXTOID;
			prev_tle->resdom->restype = TEXTOID;
		}
	}
}

/*
 * Rewrites UNION INTERSECT and EXCEPT queries to semantically equivalent
 * queries that use IN and NOT IN subselects.
 *
 * The operator tree is attached to 'intersectClause' (see rule
 * 'SelectStmt' in gram.y) of the 'parsetree' given as an
 * argument. First we remember some clauses (the sortClause, the
 * distinctClause etc.)  Then we translate the operator tree to DNF
 * (disjunctive normal form) by 'cnfify'. (Note that 'cnfify' produces
 * CNF but as we exchanged ANDs with ORs in function A_Expr_to_Expr()
 * earlier we get DNF after exchanging ANDs and ORs again in the
 * result.) Now we create a new query by evaluating the new operator
 * tree which is in DNF now. For every AND we create an entry in the
 * union list and for every OR we create an IN subselect. (NOT IN
 * subselects are created for OR NOT nodes). The first entry of the
 * union list is handed back but before that the remembered clauses
 * (sortClause etc) are attached to the new top Node (Note that the
 * new top Node can differ from the parsetree given as argument because of
 * the translation to DNF. That's why we have to remember the sortClause
 * and so on!)
 */
static Query *
Except_Intersect_Rewrite(Query *parsetree)
{

	SubLink    *n;
	Query	   *result,
			   *intersect_node;
	List	   *elist,
			   *intersect_list = NIL,
			   *intersect,
			   *intersectClause;
	List	   *union_list = NIL,
			   *sortClause,
			   *distinctClause;
	List	   *left_expr,
			   *resnames = NIL;
	char	   *op,
			   *into;
	bool		isBinary,
				isPortal,
				isTemp;
	Node	   *limitOffset,
			   *limitCount;
	CmdType		commandType = CMD_SELECT;
	RangeTblEntry *rtable_insert = NULL;
	List	   *prev_target = NIL;

	/*
	 * Remember the Resnames of the given parsetree's targetlist (these
	 * are the resnames of the first Select Statement of the query
	 * formulated by the user and he wants the columns named by these
	 * strings. The transformation to DNF can cause another Select
	 * Statment to be the top one which uses other names for its columns.
	 * Therefore we remember the original names and attach them to the
	 * targetlist of the new topmost Node at the end of this function
	 */
	foreach(elist, parsetree->targetList)
	{
		TargetEntry *tent = (TargetEntry *) lfirst(elist);

		if (! tent->resdom->resjunk)
			resnames = lappend(resnames, tent->resdom->resname);
	}

	/*
	 * If the Statement is an INSERT INTO ... (SELECT...) statement using
	 * UNIONs, INTERSECTs or EXCEPTs and the transformation to DNF makes
	 * another Node to the top node we have to transform the new top node
	 * to an INSERT node and the original INSERT node to a SELECT node
	 */
	if (parsetree->commandType == CMD_INSERT)
	{

		/*
		 * The result relation ( = the one to insert into) has to be
		 * attached to the rtable list of the new top node
		 */
		rtable_insert = rt_fetch(parsetree->resultRelation, parsetree->rtable);

		parsetree->commandType = CMD_SELECT;
		commandType = CMD_INSERT;
		parsetree->resultRelation = 0;
	}

	/*
	 * Save some items, to be able to attach them to the resulting top
	 * node at the end of the function
	 */
	sortClause = parsetree->sortClause;
	distinctClause = parsetree->distinctClause;
	into = parsetree->into;
	isBinary = parsetree->isBinary;
	isPortal = parsetree->isPortal;
	isTemp = parsetree->isTemp;
	limitOffset = parsetree->limitOffset;
	limitCount = parsetree->limitCount;

	/*
	 * The operator tree attached to parsetree->intersectClause is still
	 * 'raw' ( = the leaf nodes are still SelectStmt nodes instead of
	 * Query nodes) So step through the tree and transform the nodes using
	 * parse_analyze().
	 *
	 * The parsetree (given as an argument to Except_Intersect_Rewrite()) has
	 * already been transformed and transforming it again would cause
	 * troubles.  So we give the 'raw' version (of the cooked parsetree)
	 * to the function to prevent an additional transformation. Instead we
	 * hand back the 'cooked' version also given as an argument to
	 * intersect_tree_analyze()
	 */
	intersectClause =
		(List *) intersect_tree_analyze((Node *) parsetree->intersectClause,
								 (Node *) lfirst(parsetree->unionClause),
										(Node *) parsetree);

	/* intersectClause is no longer needed so set it to NIL */
	parsetree->intersectClause = NIL;

	/*
	 * unionClause will be needed later on but the list it delivered is no
	 * longer needed, so set it to NIL
	 */
	parsetree->unionClause = NIL;

	/*
	 * Transform the operator tree to DNF (remember ANDs and ORs have been
	 * exchanged, that's why we get DNF by using cnfify)
	 *
	 * After the call, explicit ANDs are removed and all AND operands are
	 * simply items in the intersectClause list
	 */
	intersectClause = cnfify((Expr *) intersectClause, true);

	/*
	 * For every entry of the intersectClause list we generate one entry
	 * in the union_list
	 */
	foreach(intersect, intersectClause)
	{

		/*
		 * for every OR we create an IN subselect and for every OR NOT we
		 * create a NOT IN subselect, so first extract all the Select
		 * Query nodes from the tree (that contains only OR or OR NOTs any
		 * more because we did a transformation to DNF
		 *
		 * There must be at least one node that is not negated (i.e. just OR
		 * and not OR NOT) and this node will be the first in the list
		 * returned
		 */
		intersect_list = NIL;
		create_intersect_list((Node *) lfirst(intersect), &intersect_list);

		/*
		 * This one will become the Select Query node, all other nodes are
		 * transformed into subselects under this node!
		 */
		intersect_node = (Query *) lfirst(intersect_list);
		intersect_list = lnext(intersect_list);

		/*
		 * Check if all Select Statements use the same number of
		 * attributes and if all corresponding attributes are of the same
		 * type
		 */
		if (prev_target)
			check_targetlists_are_compatible(prev_target, intersect_node->targetList);
		prev_target = intersect_node->targetList;

		/*
		 * Transform all nodes remaining into subselects and add them to
		 * the qualifications of the Select Query node
		 */
		while (intersect_list != NIL)
		{

			n = makeNode(SubLink);

			/* Here we got an OR so transform it to an IN subselect */
			if (IsA(lfirst(intersect_list), Query))
			{

				/*
				 * Check if all Select Statements use the same number of
				 * attributes and if all corresponding attributes are of
				 * the same type
				 */
				check_targetlists_are_compatible(prev_target,
						 ((Query *) lfirst(intersect_list))->targetList);

				n->subselect = lfirst(intersect_list);
				op = "=";
				n->subLinkType = ANY_SUBLINK;
				n->useor = false;
			}

			/*
			 * Here we got an OR NOT node so transform it to a NOT IN
			 * subselect
			 */
			else
			{

				/*
				 * Check if all Select Statements use the same number of
				 * attributes and if all corresponding attributes are of
				 * the same type
				 */
				check_targetlists_are_compatible(prev_target,
												 ((Query *) lfirst(((Expr *) lfirst(intersect_list))->args))->targetList);

				n->subselect = (Node *) lfirst(((Expr *) lfirst(intersect_list))->args);
				op = "<>";
				n->subLinkType = ALL_SUBLINK;
				n->useor = true;
			}

			/*
			 * Prepare the lefthand side of the Sublinks: All the entries
			 * of the targetlist must be (IN) or must not be (NOT IN) the
			 * subselect
			 */
			n->lefthand = NIL;
			foreach(elist, intersect_node->targetList)
			{
				TargetEntry *tent = (TargetEntry *) lfirst(elist);

				if (! tent->resdom->resjunk)
					n->lefthand = lappend(n->lefthand, tent->expr);
			}

			/*
			 * Also prepare the list of Opers that must be used for the
			 * comparisons (they depend on the specific datatypes
			 * involved!)
			 */
			left_expr = n->lefthand;
			n->oper = NIL;

			foreach(elist, ((Query *) (n->subselect))->targetList)
			{
				TargetEntry *tent = (TargetEntry *) lfirst(elist);
				Node	   *lexpr;
				Operator	optup;
				Form_pg_operator opform;
				Oper	   *newop;

				if (tent->resdom->resjunk)
					continue;

				lexpr = lfirst(left_expr);

				optup = oper(op,
							 exprType(lexpr),
							 exprType(tent->expr),
							 FALSE);
				opform = (Form_pg_operator) GETSTRUCT(optup);

				if (opform->oprresult != BOOLOID)
					elog(ERROR, "parser: '%s' must return 'bool' to be used with quantified predicate subquery", op);

				newop = makeOper(oprid(optup),	/* opno */
								 InvalidOid,	/* opid */
								 opform->oprresult);

				n->oper = lappend(n->oper, newop);

				left_expr = lnext(left_expr);
			}

			Assert(left_expr == NIL); /* should have used 'em all */

			/*
			 * If the Select Query node has aggregates in use add all the
			 * subselects to the HAVING qual else to the WHERE qual
			 */
			if (intersect_node->hasAggs)
				AddHavingQual(intersect_node, (Node *) n);
			else
				AddQual(intersect_node, (Node *) n);

			/* Now we got sublinks */
			intersect_node->hasSubLinks = true;
			intersect_list = lnext(intersect_list);
		}
		intersect_node->intersectClause = NIL;
		union_list = lappend(union_list, intersect_node);
	}

	/* The first entry to union_list is our new top node */
	result = (Query *) lfirst(union_list);
	/* attach the rest to unionClause */
	result->unionClause = lnext(union_list);
	/* Attach all the items remembered in the beginning of the function */
	result->sortClause = sortClause;
	result->distinctClause = distinctClause;
	result->into = into;
	result->isPortal = isPortal;
	result->isBinary = isBinary;
	result->isTemp = isTemp;
	result->limitOffset = limitOffset;
	result->limitCount = limitCount;

	/*
	 * The relation to insert into is attached to the range table of the
	 * new top node
	 */
	if (commandType == CMD_INSERT)
	{
		result->rtable = lappend(result->rtable, rtable_insert);
		result->resultRelation = length(result->rtable);
		result->commandType = commandType;
	}

	/*
	 * The resnames of the originally first SelectStatement are attached
	 * to the new first SelectStatement
	 */
	foreach(elist, result->targetList)
	{
		TargetEntry *tent = (TargetEntry *) lfirst(elist);

		if (tent->resdom->resjunk)
			continue;

		tent->resdom->resname = lfirst(resnames);
		resnames = lnext(resnames);
	}

	return result;
}

/*
 * Create a list of nodes that are either Query nodes of NOT Expr
 * nodes followed by a Query node. The tree given in ptr contains at
 * least one non negated Query node. This node is attached to the
 * beginning of the list.
 */
static void
create_intersect_list(Node *ptr, List **intersect_list)
{
	List	   *arg;

	if (IsA(ptr, Query))
	{
		/* The non negated node is attached at the beginning (lcons) */
		*intersect_list = lcons(ptr, *intersect_list);
		return;
	}

	if (IsA(ptr, Expr))
	{
		if (((Expr *) ptr)->opType == NOT_EXPR)
		{
			/* negated nodes are appended to the end (lappend) */
			*intersect_list = lappend(*intersect_list, ptr);
			return;
		}
		else
		{
			foreach(arg, ((Expr *) ptr)->args)
				create_intersect_list(lfirst(arg), intersect_list);
			return;
		}
		return;
	}
}

/*
 * The nodes given in 'tree' are still 'raw' so 'cook' them using
 * parse_analyze().  The node given in first_select has already been cooked,
 * so don't transform it again but return a pointer to the previously cooked
 * version given in 'parsetree' instead.
 */
static Node *
intersect_tree_analyze(Node *tree, Node *first_select, Node *parsetree)
{
	Node	   *result = (Node *) NIL;
	List	   *arg;

	if (IsA(tree, SelectStmt))
	{

		/*
		 * If we get to the tree given in first_select return parsetree
		 * instead of performing parse_analyze()
		 */
		if (tree == first_select)
			result = parsetree;
		else
		{
			/* transform the 'raw' nodes to 'cooked' Query nodes */
			List	   *qtree = parse_analyze(makeList1(tree), NULL);

			result = (Node *) lfirst(qtree);
		}
	}

	if (IsA(tree, Expr))
	{
		/* Call recursively for every argument of the node */
		foreach(arg, ((Expr *) tree)->args)
			lfirst(arg) = intersect_tree_analyze(lfirst(arg), first_select, parsetree);
		result = tree;
	}
	return result;
}
