/*-------------------------------------------------------------------------
 *
 * view.c
 *	  use rewrite rules to construct views
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	$Id: view.c,v 1.54 2001/03/22 03:59:25 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "catalog/heap.h"
#include "commands/creatinh.h"
#include "commands/view.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rewriteRemove.h"

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif

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
static void
DefineVirtualRelation(char *relname, List *tlist)
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
			char	   *resname = res->resname;
			char	   *restypename = typeidTypeName(res->restype);
			ColumnDef  *def = makeNode(ColumnDef);
			TypeName   *typename = makeNode(TypeName);

			def->colname = pstrdup(resname);

			typename->name = pstrdup(restypename);
			typename->typmod = res->restypmod;
			def->typename = typename;

			def->is_not_null = false;
			def->is_sequence = false;
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
	createStmt->relname = relname;
	createStmt->istemp = false;
	createStmt->tableElts = attrList;
	createStmt->inhRelnames = NIL;
	createStmt->constraints = NIL;

	/*
	 * finally create the relation...
	 */
	DefineRelation(createStmt, RELKIND_VIEW);
}

/*------------------------------------------------------------------
 * makeViewRetrieveRuleName
 *
 * Given a view name, returns the name for the 'on retrieve to "view"'
 * rule.
 *------------------------------------------------------------------
 */
char *
MakeRetrieveViewRuleName(char *viewName)
{
	char	   *buf;
	int			buflen,
				maxlen;

	buflen = strlen(viewName) + 5;
	buf = palloc(buflen);
	snprintf(buf, buflen, "_RET%s", viewName);
	/* clip to less than NAMEDATALEN bytes, if necessary */
#ifdef MULTIBYTE
	maxlen = pg_mbcliplen(buf, strlen(buf), NAMEDATALEN - 1);
#else
	maxlen = NAMEDATALEN - 1;
#endif
	if (maxlen < buflen)
		buf[maxlen] = '\0';

	return buf;
}

static RuleStmt *
FormViewRetrieveRule(char *viewName, Query *viewParse)
{
	RuleStmt   *rule;
	char	   *rname;
	Attr	   *attr;

	/*
	 * Create a RuleStmt that corresponds to the suitable rewrite rule
	 * args for DefineQueryRewrite();
	 */
	rule = makeNode(RuleStmt);
	rname = MakeRetrieveViewRuleName(viewName);

	attr = makeNode(Attr);
	attr->relname = pstrdup(viewName);
	rule->rulename = pstrdup(rname);
	rule->whereClause = NULL;
	rule->event = CMD_SELECT;
	rule->object = attr;
	rule->instead = true;
	rule->actions = makeList1(viewParse);

	return rule;
}

static void
DefineViewRules(char *viewName, Query *viewParse)
{
	RuleStmt   *retrieve_rule;

#ifdef NOTYET
	RuleStmt   *replace_rule;
	RuleStmt   *append_rule;
	RuleStmt   *delete_rule;

#endif

	retrieve_rule = FormViewRetrieveRule(viewName, viewParse);

#ifdef NOTYET

	replace_rule = FormViewReplaceRule(viewName, viewParse);
	append_rule = FormViewAppendRule(viewName, viewParse);
	delete_rule = FormViewDeleteRule(viewName, viewParse);

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
UpdateRangeTableOfViewParse(char *viewName, Query *viewParse)
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
	rt_entry1 = addRangeTableEntry(NULL, viewName,
								   makeAttr("*OLD*", NULL),
								   false, false);
	rt_entry2 = addRangeTableEntry(NULL, viewName,
								   makeAttr("*NEW*", NULL),
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
DefineView(char *viewName, Query *viewParse)
{

	/*
	 * Create the "view" relation NOTE: if it already exists, the xact
	 * will be aborted.
	 */
	DefineVirtualRelation(viewName, viewParse->targetList);

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
	viewParse = UpdateRangeTableOfViewParse(viewName, viewParse);

	/*
	 * Now create the rules associated with the view.
	 */
	DefineViewRules(viewName, viewParse);
}

/*------------------------------------------------------------------
 * RemoveView
 *
 * Remove a view given its name
 *------------------------------------------------------------------
 */
void
RemoveView(char *viewName)
{

	/*
	 * We just have to drop the relation; the associated rules will be
	 * cleaned up automatically.
	 */
	heap_drop_with_catalog(viewName, allowSystemTableMods);
}
