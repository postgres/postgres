/*-------------------------------------------------------------------------
 *
 * rewriteHandler.c--
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteHandler.c,v 1.18 1998/08/18 00:48:59 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"
#include "miscadmin.h"
#include "utils/palloc.h"
#include "utils/elog.h"
#include "utils/rel.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"

#include "parser/parsetree.h"	/* for parsetree manipulation */
#include "parser/parse_relation.h"
#include "nodes/parsenodes.h"

#include "rewrite/rewriteSupport.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/locks.h"

#include "commands/creatinh.h"
#include "access/heapam.h"

#include "utils/syscache.h"
#include "utils/acl.h"
#include "catalog/pg_shadow.h"

static void
ApplyRetrieveRule(Query *parsetree, RewriteRule *rule,
				  int rt_index, int relation_level,
				  Relation relation, int *modified);
static List *
fireRules(Query *parsetree, int rt_index, CmdType event,
		  bool *instead_flag, List *locks, List **qual_products);
static void QueryRewriteSubLink(Node *node);
static List *QueryRewriteOne(Query *parsetree);
static List *deepRewriteQuery(Query *parsetree);
static void CheckViewPerms(Relation view, List *rtable);
static void RewritePreprocessQuery(Query *parsetree);
static Query *RewritePostprocessNonSelect(Query *parsetree);

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
				  bool *instead_flag)
{
	RewriteInfo *info;
	int			rt_length;
	int			result_reln;

	info = (RewriteInfo *) palloc(sizeof(RewriteInfo));
	info->rt_index = rt_index;
	info->event = event;
	info->instead_flag = *instead_flag;
	info->rule_action = (Query *) copyObject(rule_action);
	info->rule_qual = (Node *) copyObject(rule_qual);
	if (info->rule_action == NULL)
		info->nothing = TRUE;
	else
	{
		info->nothing = FALSE;
		info->action = info->rule_action->commandType;
		info->current_varno = rt_index;
		info->rt = parsetree->rtable;
		rt_length = length(info->rt);
		info->rt = append(info->rt, info->rule_action->rtable);

		info->new_varno = PRS2_NEW_VARNO + rt_length;
		OffsetVarNodes(info->rule_action->qual, rt_length);
		OffsetVarNodes((Node *) info->rule_action->targetList, rt_length);
		OffsetVarNodes(info->rule_qual, rt_length);
		ChangeVarNodes((Node *) info->rule_action->qual,
					   PRS2_CURRENT_VARNO + rt_length, rt_index, 0);
		ChangeVarNodes((Node *) info->rule_action->targetList,
					   PRS2_CURRENT_VARNO + rt_length, rt_index, 0);
		ChangeVarNodes(info->rule_qual,
					   PRS2_CURRENT_VARNO + rt_length, rt_index, 0);

		/*
		 * bug here about replace CURRENT  -- sort of replace current is
		 * deprecated now so this code shouldn't really need to be so
		 * clutzy but.....
		 */
		if (info->action != CMD_SELECT)
		{						/* i.e update XXXXX */
			int			new_result_reln = 0;

			result_reln = info->rule_action->resultRelation;
			switch (result_reln)
			{
				case PRS2_CURRENT_VARNO:
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

static List *
OptimizeRIRRules(List *locks)
{
	List	   *attr_level = NIL,
			   *i;
	List	   *relation_level = NIL;

	foreach(i, locks)
	{
		RewriteRule *rule_lock = lfirst(i);

		if (rule_lock->attrno == -1)
			relation_level = lappend(relation_level, rule_lock);
		else
			attr_level = lappend(attr_level, rule_lock);
	}
	return nconc(relation_level, attr_level);
}

/*
 * idea is to fire regular rules first, then qualified instead
 * rules and unqualified instead rules last. Any lemming is counted for.
 */
static List *
orderRules(List *locks)
{
	List	*regular = NIL;
	List	*instead_rules = NIL;
	List	*instead_qualified = NIL;
	List	*i;

	foreach(i, locks)
	{
		RewriteRule *rule_lock = (RewriteRule *) lfirst(i);

		if (rule_lock->isInstead) {
			if (rule_lock->qual == NULL)
				instead_rules = lappend(instead_rules, rule_lock);
			else
				instead_qualified = lappend(instead_qualified, rule_lock);
		} else
			regular = lappend(regular, rule_lock);
	}
	regular = nconc(regular, instead_qualified);
	return nconc(regular, instead_rules);
}

static int
AllRetrieve(List *actions)
{
	List	   *n;

	foreach(n, actions)
	{
		Query	   *pt = lfirst(n);

		/*
		 * in the old postgres code, we check whether command_type is a
		 * consp of '('*'.commandType). but we've never supported
		 * transitive closures. Hence removed	 - ay 10/94.
		 */
		if (pt->commandType != CMD_SELECT)
			return false;
	}
	return true;
}

static List *
FireRetrieveRulesAtQuery(Query *parsetree,
						 int rt_index,
						 Relation relation,
						 bool *instead_flag,
						 int rule_flag)
{
	List	   *i,
			   *locks;
	RuleLock   *rt_entry_locks = NULL;
	List	   *work = NIL;

	if ((rt_entry_locks = relation->rd_rules) == NULL)
		return NIL;

	locks = matchLocks(CMD_SELECT, rt_entry_locks, rt_index, parsetree);	

	/* find all retrieve instead */
	foreach(i, locks)
	{
		RewriteRule *rule_lock = (RewriteRule *) lfirst(i);

		if (!rule_lock->isInstead)
			continue;
		work = lappend(work, rule_lock);
	}
	if (work != NIL)
	{
		work = OptimizeRIRRules(locks);
		foreach(i, work)
		{
			RewriteRule *rule_lock = lfirst(i);
			int			relation_level;
			int			modified = FALSE;

			relation_level = (rule_lock->attrno == -1);
			if (rule_lock->actions == NIL)
			{
				*instead_flag = TRUE;
				return NIL;
			}
			if (!rule_flag &&
				length(rule_lock->actions) >= 2 &&
				AllRetrieve(rule_lock->actions))
			{
				*instead_flag = TRUE;
				return rule_lock->actions;
			}
			ApplyRetrieveRule(parsetree, rule_lock, rt_index, relation_level, relation,
							  &modified);
			if (modified)
			{
				*instead_flag = TRUE;
				FixResdomTypes(parsetree->targetList);
				return lcons(parsetree, NIL);
			}
		}
	}
	return NIL;
}


/* Idea is like this:
 *
 * retrieve-instead-retrieve rules have different semantics than update nodes
 * Separate RIR rules from others.	Pass others to FireRules.
 * Order RIR rules and process.
 *
 * side effect: parsetree's rtable field might be changed
 */
static void
ApplyRetrieveRule(Query *parsetree,
				  RewriteRule *rule,
				  int rt_index,
				  int relation_level,
				  Relation relation,
				  int *modified)
{
	Query	   *rule_action = NULL;
	Node	   *rule_qual;
	List	   *rtable,
			   *rt;
	int			nothing,
				rt_length;
	int			badsql = FALSE;
	int			viewAclOverride = FALSE;

	rule_qual = rule->qual;
	if (rule->actions)
	{
		if (length(rule->actions) > 1)	/* ??? because we don't handle
										 * rules with more than one
										 * action? -ay */

			/*
			 * WARNING!!! If we sometimes handle rules with more than one
			 * action, the view acl checks might get broken.
			 * viewAclOverride should only become true (below) if this is
			 * a relation_level, instead, select query - Jan
			 */
			return;
		rule_action = copyObject(lfirst(rule->actions));
		nothing = FALSE;

		/*
		 * If this rule is on the relation level, the rule action is a
		 * select and the rule is instead then it must be a view.
		 * Permissions for views now follow the owner of the view, not the
		 * current user.
		 */
		if (relation_level && rule_action->commandType == CMD_SELECT
			&& rule->isInstead)
		{
			CheckViewPerms(relation, rule_action->rtable);
			viewAclOverride = TRUE;
		}
	}
	else
		nothing = TRUE;

	rtable = copyObject(parsetree->rtable);
	foreach(rt, rtable)
	{
		RangeTblEntry *rte = lfirst(rt);

		/*
		 * this is to prevent add_missing_vars_to_base_rels() from adding
		 * a bogus entry to the new target list.
		 */
		rte->inFromCl = false;
	}
	rt_length = length(rtable);

	if (viewAclOverride)
	{
		List	   *rule_rtable,
				   *rule_rt;
		RangeTblEntry *rte;

		rule_rtable = copyObject(rule_action->rtable);
		foreach(rule_rt, rule_rtable)
		{
			rte = lfirst(rule_rt);

			/*
			 * tell the executor that the ACL check on this range table
			 * entry is already done
			 */
			rte->skipAcl = true;
		}

		rtable = nconc(rtable, rule_rtable);
	}
	else
		rtable = nconc(rtable, copyObject(rule_action->rtable));
	parsetree->rtable = rtable;

	rule_action->rtable = rtable;
	OffsetVarNodes(rule_action->qual, rt_length);
	OffsetVarNodes((Node *) rule_action->targetList, rt_length);
	OffsetVarNodes(rule_qual, rt_length);
	
	OffsetVarNodes((Node *) rule_action->groupClause, rt_length);
	OffsetVarNodes((Node *) rule_action->havingQual, rt_length);

	ChangeVarNodes(rule_action->qual,
				   PRS2_CURRENT_VARNO + rt_length, rt_index, 0);
	ChangeVarNodes((Node *) rule_action->targetList,
				   PRS2_CURRENT_VARNO + rt_length, rt_index, 0);
	ChangeVarNodes(rule_qual, PRS2_CURRENT_VARNO + rt_length, rt_index, 0);

	ChangeVarNodes((Node *) rule_action->groupClause,
				   PRS2_CURRENT_VARNO + rt_length, rt_index, 0);
	ChangeVarNodes((Node *) rule_action->havingQual,
				   PRS2_CURRENT_VARNO + rt_length, rt_index, 0);

	if (relation_level)
	{
	  HandleViewRule(parsetree, rtable, rule_action->targetList, rt_index,
			 modified);
	}
	else
	{
	  HandleRIRAttributeRule(parsetree, rtable, rule_action->targetList,
				 rt_index, rule->attrno, modified, &badsql);
	}
	if (*modified && !badsql) {
	  AddQual(parsetree, rule_action->qual);
	  /* This will only work if the query made to the view defined by the following
	   * groupClause groups by the same attributes or does not use group at all! */
	  if (parsetree->groupClause == NULL)
	    parsetree->groupClause=rule_action->groupClause;
	  AddHavingQual(parsetree, rule_action->havingQual);
	  parsetree->hasAggs = (rule_action->hasAggs || parsetree->hasAggs);
	  parsetree->hasSubLinks = (rule_action->hasSubLinks ||  parsetree->hasSubLinks);
	}	
}

static List *
ProcessRetrieveQuery(Query *parsetree,
					 List *rtable,
					 bool *instead_flag,
					 bool rule)
{
	List	   *rt;
	List	   *product_queries = NIL;
	int			rt_index = 0;


	foreach(rt, rtable)
	{
		RangeTblEntry *rt_entry = lfirst(rt);
		Relation	rt_entry_relation = NULL;
		List	   *result = NIL;

		rt_index++;
		rt_entry_relation = heap_openr(rt_entry->relname);



		if (rt_entry_relation->rd_rules != NULL)
		{
			result =
				FireRetrieveRulesAtQuery(parsetree,
										 rt_index,
										 rt_entry_relation,
										 instead_flag,
										 rule);
		}
		heap_close(rt_entry_relation);
		if (*instead_flag) {
			return result;
		}
	}
	if (rule)
		return NIL;

	foreach(rt, rtable)
	{
		RangeTblEntry *rt_entry = lfirst(rt);
		Relation	rt_entry_relation = NULL;
		RuleLock   *rt_entry_locks = NULL;
		List	   *result = NIL;
		List	   *locks = NIL;
		List	   *dummy_products;

		rt_index++;
		rt_entry_relation = heap_openr(rt_entry->relname);
		rt_entry_locks = rt_entry_relation->rd_rules;
		heap_close(rt_entry_relation);


		if (rt_entry_locks)
		{
			locks =
				matchLocks(CMD_SELECT, rt_entry_locks, rt_index, parsetree);
		}
		if (locks != NIL)
		{
			result = fireRules(parsetree, rt_index, CMD_SELECT,
							   instead_flag, locks, &dummy_products);
			if (*instead_flag)
				return lappend(NIL, result);
			if (result != NIL)
				product_queries = nconc(product_queries, result);
		}
	}
	return product_queries;
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

		rtable = new_tree->rtable;
		rt_length = length(rtable);
		rtable = append(rtable, listCopy(rule_action->rtable));
		new_tree->rtable = rtable;
		OffsetVarNodes(new_qual, rt_length);
		ChangeVarNodes(new_qual, PRS2_CURRENT_VARNO + rt_length, rt_index, 0);
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
	{
		ProcessRetrieveQuery(parsetree,
							 parsetree->rtable,
							 instead_flag, TRUE);
		if (*instead_flag)
			return lappend(NIL, parsetree);
		else
			return NIL;
	}

	locks = orderRules(locks);	/* real instead rules last */
	foreach(i, locks)
	{
		RewriteRule *rule_lock = (RewriteRule *) lfirst(i);
		Node	   *qual,
				   *event_qual;
		List	   *actions;
		List	   *r;
		bool		orig_instead_flag = *instead_flag;

		/* multiple rule action time */
		*instead_flag = rule_lock->isInstead;
		event_qual = rule_lock->qual;
		actions = rule_lock->actions;
		if (event_qual != NULL && *instead_flag) {
			Query		*qual_product;
			RewriteInfo	qual_info;

			/* ----------
			 * If there are instead rules with qualifications,
			 * the original query is still performed. But all
			 * the negated rule qualifications of the instead
			 * rules are added so it does it's actions only
			 * in cases where the rule quals of all instead
			 * rules are false. Think of it as the default
			 * action in a case. We save this in *qual_products
			 * so deepRewriteQuery() can add it to the query
			 * list after we mangled it up enough.
			 * ----------
			 */
			if (*qual_products == NIL) {
				qual_product = parsetree;
			} else {
				qual_product = (Query *)nth(0, *qual_products);
			}

			qual_info.event		= qual_product->commandType;
			qual_info.new_varno	= length(qual_product->rtable) + 2;
			qual_product = CopyAndAddQual(qual_product, 
					actions, 
					event_qual,
					rt_index,
					event);
			
			qual_info.rule_action	= qual_product;

			if (event == CMD_INSERT || event == CMD_UPDATE)
				FixNew(&qual_info, qual_product);

			*qual_products = lappend(NIL, qual_product);
		}

		foreach(r, actions)
		{
			Query	   *rule_action = lfirst(r);
			Node	   *rule_qual = copyObject(event_qual);

			if (rule_action->commandType == CMD_NOTHING)
				continue;

			/*--------------------------------------------------
			 * Step 1:
			 *	  Rewrite current.attribute or current to tuple variable
			 *	  this appears to be done in parser?
			 *--------------------------------------------------
			 */
			info = gatherRewriteMeta(parsetree, rule_action, rule_qual,
									 rt_index, event, instead_flag);

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
			qual = parsetree->qual;
			AddQual(info->rule_action, qual);

			if (info->rule_qual != NULL)
				AddQual(info->rule_action, info->rule_qual);

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
			 *	  rewriting due to retrieve rules
			 *--------------------------------------------------
			 */
			info->rule_action->rtable = info->rt;
			ProcessRetrieveQuery(info->rule_action, info->rt,
								 &orig_instead_flag, TRUE);

			/*--------------------------------------------------
			 * Step 4
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
		if (event_qual == NULL && *instead_flag) {
			*qual_products = NIL;
		}
	}
	return results;
}

/* ----------
 * RewritePreprocessQuery -
 *	adjust details in the parsetree, the rule system
 *	depends on
 * ----------
 */
static void
RewritePreprocessQuery(Query *parsetree)
{
	/* ----------
	 * if the query has a resultRelation, reassign the
	 * result domain numbers to the attribute numbers in the
	 * target relation. FixNew() depends on it when replacing
	 * *new* references in a rule action by the expressions
	 * from the rewritten query.
	 * ----------
	 */
	if (parsetree->resultRelation > 0) {
		RangeTblEntry	*rte;
		Relation	rd;
		List		*tl;
		TargetEntry	*tle;
		int		resdomno;
	
		rte = (RangeTblEntry *)nth(parsetree->resultRelation - 1,
					parsetree->rtable);
		rd = heap_openr(rte->relname);

		foreach (tl, parsetree->targetList) {
			tle = (TargetEntry *)lfirst(tl);
			resdomno = attnameAttNum(rd, tle->resdom->resname);
			tle->resdom->resno = resdomno;
		}

		heap_close(rd);
	}
}


/* ----------
 * RewritePostprocessNonSelect -
 *	apply instead select rules on a query fired in by
 *	the rewrite system
 * ----------
 */
static Query *
RewritePostprocessNonSelect(Query *parsetree)
{
	List		*rt;
	int		rt_index = 0;
	Query		*newtree = copyObject(parsetree);
	
	foreach(rt, parsetree->rtable)
	{
		RangeTblEntry	*rt_entry = lfirst(rt);
		Relation	rt_entry_relation = NULL;
		RuleLock	*rt_entry_locks = NULL;
		List		*locks = NIL;
		List		*instead_locks = NIL;
		List		*lock;
		RewriteRule	*rule;

		rt_index++;
		rt_entry_relation = heap_openr(rt_entry->relname);
		rt_entry_locks = rt_entry_relation->rd_rules;

		if (rt_entry_locks)
		{
			int	origcmdtype = newtree->commandType;
			newtree->commandType = CMD_SELECT;
			locks =
				matchLocks(CMD_SELECT, rt_entry_locks, rt_index, newtree);
			newtree->commandType = origcmdtype;
		}
		if (locks != NIL)
		{
			foreach (lock, locks) {
				rule = (RewriteRule *)lfirst(lock);
				if (rule->isInstead) {
					instead_locks = nconc(instead_locks, lock);
				}
			}
		}
		if (instead_locks != NIL)
		{
			foreach (lock, instead_locks) {
				int	relation_level;
				int	modified = 0;

				rule = (RewriteRule *)lfirst(lock);
				relation_level = (rule->attrno == -1);

				ApplyRetrieveRule(newtree,
					rule,
					rt_index,
					relation_level,
					rt_entry_relation,
					&modified);
			}
		}

		heap_close(rt_entry_relation);
	}

	return newtree;
}

static List *
RewriteQuery(Query *parsetree, bool *instead_flag, List **qual_products)
{
	CmdType		event;
	List	   *product_queries = NIL;
	int			result_relation = 0;

	Assert(parsetree != NULL);

	event = parsetree->commandType;

	if (event == CMD_UTILITY)
		return NIL;

	/*
	 * only for a delete may the targetlist be NULL
	 */
	if (event != CMD_DELETE)
		Assert(parsetree->targetList != NULL);

	result_relation = parsetree->resultRelation;

	if (event != CMD_SELECT)
	{

		/*
		 * the statement is an update, insert or delete
		 */
		RangeTblEntry *rt_entry;
		Relation	rt_entry_relation = NULL;
		RuleLock   *rt_entry_locks = NULL;

		rt_entry = rt_fetch(result_relation, parsetree->rtable);
		rt_entry_relation = heap_openr(rt_entry->relname);
		rt_entry_locks = rt_entry_relation->rd_rules;
		heap_close(rt_entry_relation);

		if (rt_entry_locks != NULL)
		{
			List	   *locks =
			matchLocks(event, rt_entry_locks, result_relation, parsetree);
			product_queries =
				fireRules(parsetree,
						  result_relation,
						  event,
						  instead_flag,
						  locks,
						  qual_products);
		}

		/* ----------
		 * deepRewriteQuery does not handle the situation
		 * where a query fired by a rule uses relations that
		 * have instead select rules defined (views and the like).
		 * So we care for them here.
		 * ----------
		 */
		if (product_queries != NIL) {
			List	*pq;
			Query	*tmp;
			List	*new_products = NIL;
		
			foreach (pq, product_queries) {
				tmp = (Query *)lfirst(pq);
				tmp = RewritePostprocessNonSelect(tmp);
				new_products = lappend(new_products, tmp);
		    	}
			product_queries = new_products;
		}

		return product_queries;
	}
	else
	{

		/*
		 * the statement is a select
		 */
		Query	   *other;

		/*
		 * ApplyRetrieveRule changes the range table XXX Unions are copied
		 * again.
		 */
		other = copyObject(parsetree);

		return
			ProcessRetrieveQuery(other, parsetree->rtable,
								 instead_flag, FALSE);
	}
}

/*
 * to avoid infinite recursion, we restrict the number of times a query
 * can be rewritten. Detecting cycles is left for the reader as an excercise.
 */
#ifndef REWRITE_INVOKE_MAX
#define REWRITE_INVOKE_MAX		10
#endif

static int	numQueryRewriteInvoked = 0;

/*
 * QueryRewrite -
 *	  rewrite one query via QueryRewrite system, possibly returning 0, or many
 *	  queries
 */
List *
QueryRewrite(Query *parsetree)
{
	RewritePreprocessQuery(parsetree);

	QueryRewriteSubLink(parsetree->qual);
	QueryRewriteSubLink(parsetree->havingQual);

	return QueryRewriteOne(parsetree);
}

/*
 *	QueryRewriteSubLink
 *
 *	This rewrites the SubLink subqueries first, doing the lowest ones first.
 *	We already have code in the main rewrite loops to process correlated
 *	variables from upper queries that exist in subqueries.
 */
static void
QueryRewriteSubLink(Node *node)
{
	if (node == NULL)
		return;

	switch (nodeTag(node))
	{
		case T_TargetEntry:
			break;
		case T_Aggreg:
			break;
		case T_Expr:
			{
				Expr	   *expr = (Expr *) node;

				QueryRewriteSubLink((Node *) expr->args);
			}
			break;
		case T_Var:
			break;
		case T_List:
			{
				List	   *l;

				foreach(l, (List *) node)
					QueryRewriteSubLink(lfirst(l));
			}
			break;
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;
				Query	   *query = (Query *) sublink->subselect;
				List	   *ret;

				/*
				 * Nest down first.  We do this so if a rewrite adds a
				 * SubLink we don't process it as part of this loop.
				 */
				QueryRewriteSubLink((Node *) query->qual);
				
				QueryRewriteSubLink((Node *) query->havingQual);

				ret = QueryRewriteOne(query);
				if (!ret)
					sublink->subselect = NULL;
				else if (lnext(ret) == NIL)
					sublink->subselect = lfirst(ret);
				else
					elog(ERROR, "Don't know how to process subquery that rewrites to multiple queries.");
			}
			break;
		default:
			/* ignore the others */
			break;
	}
	return;
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
 * deepRewriteQuery -
 *	  rewrites the query and apply the rules again on the queries rewritten
 */
static List *
deepRewriteQuery(Query *parsetree)
{
	List	   *n;
	List	   *rewritten = NIL;
	List	   *result = NIL;
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
		List	   *newstuff = NIL;

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
	 * The original query is appended last if not instead
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


static void
CheckViewPerms(Relation view, List *rtable)
{
	HeapTuple	utup;
	NameData	uname;
	List	   *rt;
	RangeTblEntry *rte;
	int32		aclcheck_res;

	/*
	 * get the usename of the view's owner
	 */
	utup = SearchSysCacheTuple(USESYSID, view->rd_rel->relowner, 0, 0, 0);
	if (!HeapTupleIsValid(utup))
	{
		elog(ERROR, "cache lookup for userid %d failed",
			 view->rd_rel->relowner);
	}
	StrNCpy(uname.data,
			((Form_pg_shadow) GETSTRUCT(utup))->usename.data,
			NAMEDATALEN);

	/*
	 * check that we have read access to all the classes in the range
	 * table of the view
	 */
	foreach(rt, rtable)
	{
		rte = (RangeTblEntry *) lfirst(rt);

		aclcheck_res = pg_aclcheck(rte->relname, uname.data, ACL_RD);
		if (aclcheck_res != ACLCHECK_OK)
			elog(ERROR, "%s: %s", rte->relname, aclcheck_error_strings[aclcheck_res]);
	}
}
