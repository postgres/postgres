/*-------------------------------------------------------------------------
 *
 * view.c
 *	  use rewrite rules to construct views
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	$Id: view.c,v 1.62 2002/04/15 05:22:03 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "commands/tablecmds.h"
#include "commands/view.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rewriteRemove.h"
#include "rewrite/rewriteSupport.h"
#include "utils/syscache.h"


/*---------------------------------------------------------------------
 * DefineVirtualRelation
 *
 * Create the "view" relation.
 * `DefineRelation' does all the work, we just provide the correct
 * arguments!
 *
 * If the relation already exists, then 'DefineRelation' will abort
 * the xact...
 *---------------------------------------------------------------------
 */
static Oid
DefineVirtualRelation(const RangeVar *relation, List *tlist)
{
	CreateStmt *createStmt = makeNode(CreateStmt);
	List	   *attrList,
			   *t;

	/*
	 * create a list of ColumnDef nodes based on the names and types of
	 * the (non-junk) targetlist items from the view's SELECT list.
	 */
	attrList = NIL;
	foreach(t, tlist)
	{
		TargetEntry *entry = lfirst(t);
		Resdom	   *res = entry->resdom;

		if (!res->resjunk)
		{
			ColumnDef  *def = makeNode(ColumnDef);
			TypeName   *typename = makeNode(TypeName);

			def->colname = pstrdup(res->resname);

			typename->typeid = res->restype;
			typename->typmod = res->restypmod;
			def->typename = typename;

			def->is_not_null = false;
			def->raw_default = NULL;
			def->cooked_default = NULL;
			def->constraints = NIL;

			attrList = lappend(attrList, def);
		}
	}

	if (attrList == NIL)
		elog(ERROR, "attempted to define virtual relation with no attrs");

	/*
	 * now create the parameters for keys/inheritance etc. All of them are
	 * nil...
	 */
	createStmt->relation = (RangeVar *) relation;
	createStmt->tableElts = attrList;
	createStmt->inhRelations = NIL;
	createStmt->constraints = NIL;
	createStmt->hasoids = false;

	/*
	 * finally create the relation...
	 */
	return DefineRelation(createStmt, RELKIND_VIEW);
}

static RuleStmt *
FormViewRetrieveRule(const RangeVar *view, Query *viewParse)
{
	RuleStmt   *rule;
	char	   *rname;

	/*
	 * Create a RuleStmt that corresponds to the suitable rewrite rule
	 * args for DefineQueryRewrite();
	 */
	rname = MakeRetrieveViewRuleName(view->relname);

	rule = makeNode(RuleStmt);
	rule->relation = copyObject((RangeVar *) view);
	rule->rulename = pstrdup(rname);
	rule->whereClause = NULL;
	rule->event = CMD_SELECT;
	rule->instead = true;
	rule->actions = makeList1(viewParse);

	return rule;
}

static void
DefineViewRules(const RangeVar *view, Query *viewParse)
{
	RuleStmt   *retrieve_rule;

#ifdef NOTYET
	RuleStmt   *replace_rule;
	RuleStmt   *append_rule;
	RuleStmt   *delete_rule;
#endif

	retrieve_rule = FormViewRetrieveRule(view, viewParse);

#ifdef NOTYET

	replace_rule = FormViewReplaceRule(view, viewParse);
	append_rule = FormViewAppendRule(view, viewParse);
	delete_rule = FormViewDeleteRule(view, viewParse);
#endif

	DefineQueryRewrite(retrieve_rule);

#ifdef NOTYET
	DefineQueryRewrite(replace_rule);
	DefineQueryRewrite(append_rule);
	DefineQueryRewrite(delete_rule);
#endif

}

/*---------------------------------------------------------------
 * UpdateRangeTableOfViewParse
 *
 * Update the range table of the given parsetree.
 * This update consists of adding two new entries IN THE BEGINNING
 * of the range table (otherwise the rule system will die a slow,
 * horrible and painful death, and we do not want that now, do we?)
 * one for the OLD relation and one for the NEW one (both of
 * them refer in fact to the "view" relation).
 *
 * Of course we must also increase the 'varnos' of all the Var nodes
 * by 2...
 *
 * These extra RT entries are not actually used in the query,
 * except for run-time permission checking.
 *---------------------------------------------------------------
 */
static Query *
UpdateRangeTableOfViewParse(Oid viewOid, Query *viewParse)
{
	List	   *new_rt;
	RangeTblEntry *rt_entry1,
			   *rt_entry2;

	/*
	 * Make a copy of the given parsetree.	It's not so much that we don't
	 * want to scribble on our input, it's that the parser has a bad habit
	 * of outputting multiple links to the same subtree for constructs
	 * like BETWEEN, and we mustn't have OffsetVarNodes increment the
	 * varno of a Var node twice.  copyObject will expand any
	 * multiply-referenced subtree into multiple copies.
	 */
	viewParse = (Query *) copyObject(viewParse);

	/*
	 * Create the 2 new range table entries and form the new range
	 * table... OLD first, then NEW....
	 */
	rt_entry1 = addRangeTableEntryForRelation(NULL, viewOid,
											  makeAlias("*OLD*", NIL),
											  false, false);
	rt_entry2 = addRangeTableEntryForRelation(NULL, viewOid,
											  makeAlias("*NEW*", NIL),
											  false, false);
	/* Must override addRangeTableEntry's default access-check flags */
	rt_entry1->checkForRead = false;
	rt_entry2->checkForRead = false;

	new_rt = lcons(rt_entry1, lcons(rt_entry2, viewParse->rtable));

	viewParse->rtable = new_rt;

	/*
	 * Now offset all var nodes by 2, and jointree RT indexes too.
	 */
	OffsetVarNodes((Node *) viewParse, 2, 0);

	return viewParse;
}

/*-------------------------------------------------------------------
 * DefineView
 *
 *		- takes a "viewname", "parsetree" pair and then
 *		1)		construct the "virtual" relation
 *		2)		commit the command but NOT the transaction,
 *				so that the relation exists
 *				before the rules are defined.
 *		2)		define the "n" rules specified in the PRS2 paper
 *				over the "virtual" relation
 *-------------------------------------------------------------------
 */
void
DefineView(const RangeVar *view, Query *viewParse)
{
	Oid			viewOid;

	/*
	 * Create the view relation
	 *
	 * NOTE: if it already exists, the xact will be aborted.
	 */
	viewOid = DefineVirtualRelation(view, viewParse->targetList);

	/*
	 * The relation we have just created is not visible to any other
	 * commands running with the same transaction & command id. So,
	 * increment the command id counter (but do NOT pfree any memory!!!!)
	 */
	CommandCounterIncrement();

	/*
	 * The range table of 'viewParse' does not contain entries for the
	 * "OLD" and "NEW" relations. So... add them!
	 */
	viewParse = UpdateRangeTableOfViewParse(viewOid, viewParse);

	/*
	 * Now create the rules associated with the view.
	 */
	DefineViewRules(view, viewParse);
}

/*------------------------------------------------------------------
 * RemoveView
 *
 * Remove a view given its name
 *------------------------------------------------------------------
 */
void
RemoveView(const RangeVar *view)
{
	Oid			viewOid;

	viewOid = RangeVarGetRelid(view, false);
	/*
	 * We just have to drop the relation; the associated rules will be
	 * cleaned up automatically.
	 */
	heap_drop_with_catalog(viewOid, allowSystemTableMods);
}
