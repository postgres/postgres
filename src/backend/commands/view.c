/*-------------------------------------------------------------------------
 *
 * view.c--
 *    use rewrite rules to construct views
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/commands/view.c,v 1.4 1996/11/06 08:21:43 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>	/* for sprintf() */

#include <postgres.h>

#include <catalog/heap.h>
#include <access/heapam.h>
#include <access/xact.h>
#include <utils/builtins.h>
#include <nodes/relation.h>
#include <parser/catalog_utils.h>
#include <parser/parse_query.h>
#include <rewrite/rewriteDefine.h>
#include <rewrite/rewriteHandler.h>
#include <rewrite/rewriteManip.h>
#include <rewrite/rewriteRemove.h>
#include <commands/creatinh.h>

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
    CreateStmt createStmt;
    List *attrList, *t;
    TargetEntry *entry;
    Resdom  *res;
    char *resname;
    char *restypename;
    
    /*
     * create a list with one entry per attribute of this relation.
     * Each entry is a two element list. The first element is the
     * name of the attribute (a string) and the second the name of the type
     * (NOTE: a string, not a type id!).
     */
    attrList = NIL;
    if (tlist!=NIL) {
	foreach (t, tlist ) {
	    ColumnDef *def = makeNode(ColumnDef);
	    TypeName *typename;

	    /*
	     * find the names of the attribute & its type
	     */
	    entry = lfirst(t);
	    res   = entry->resdom;
	    resname = res->resname;
	    restypename = tname(get_id_type((long)res->restype));

	    typename = makeNode(TypeName);

	    typename->name = pstrdup(restypename);
	    def->colname = pstrdup(resname);

	    def->typename = typename;

	    attrList = lappend(attrList, def);
	}
    } else {
	elog ( WARN, "attempted to define virtual relation with no attrs");
    }
    
    /*
     * now create the parametesr for keys/inheritance etc.
     * All of them are nil...
     */
    createStmt.relname = relname;
    createStmt.tableElts = attrList;
/*    createStmt.tableType = NULL;*/
    createStmt.inhRelnames = NIL;
    createStmt.archiveType = ARCH_NONE;
    createStmt.location = -1;
    createStmt.archiveLoc = -1;

    /*
     * finally create the relation...
     */
    DefineRelation(&createStmt);
}    

/*------------------------------------------------------------------
 * makeViewRetrieveRuleName
 *
 * Given a view name, returns the name for the 'on retrieve to "view"'
 * rule.
 * This routine is called when defining/removing a view.
 *
 * NOTE: it quarantees that the name is at most 15 chars long
 *
 * XXX it also means viewName cannot be 16 chars long! - ay 11/94
 *------------------------------------------------------------------
 */
char *
MakeRetrieveViewRuleName(char *viewName)
{
/*
    char buf[100];

    memset(buf, 0, sizeof(buf));
    sprintf(buf, "_RET%.*s", NAMEDATALEN, viewName->data);
    buf[15] = '\0';
    namestrcpy(rule_name, buf);
*/

    char *buf;
    buf = palloc(strlen(viewName) + 5);
    sprintf(buf, "_RET%s",viewName);
    return buf;
}

static RuleStmt *
FormViewRetrieveRule(char *viewName, Query *viewParse)
{
    RuleStmt *rule;
    char *rname;
    Attr *attr;
    
    /*
     * Create a RuleStmt that corresponds to the suitable
     * rewrite rule args for DefineQueryRewrite();
     */
    rule = makeNode(RuleStmt);
    rname = MakeRetrieveViewRuleName(viewName);

    attr = makeNode(Attr);
    attr->relname = pstrdup(viewName);
/*    attr->refname = pstrdup(viewName);*/
    rule->rulename = pstrdup(rname);
    rule->whereClause = NULL;
    rule->event = CMD_SELECT;
    rule->object = attr;
    rule->instead = true;
    rule->actions = lcons(viewParse, NIL);
	
    return rule;
}

static void
DefineViewRules(char *viewName, Query *viewParse)
{
    RuleStmt *retrieve_rule	= NULL;
#ifdef NOTYET
    RuleStmt *replace_rule	= NULL;
    RuleStmt *append_rule	= NULL;
    RuleStmt *delete_rule	= NULL;
#endif
    
    retrieve_rule = 
	FormViewRetrieveRule(viewName, viewParse);
    
#ifdef NOTYET
    
    replace_rule =
	FormViewReplaceRule(viewName, viewParse);
    append_rule = 
	FormViewAppendRule(viewName, viewParse);
    delete_rule = 
	FormViewDeleteRule(viewName, viewParse);
    
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
 * one for the CURRENT relation and one for the NEW one (both of
 * them refer in fact to the "view" relation).
 *
 * Of course we must also increase the 'varnos' of all the Var nodes
 * by 2...
 *
 * NOTE: these are destructive changes. It would be difficult to
 * make a complete copy of the parse tree and make the changes
 * in the copy.
 *---------------------------------------------------------------
 */
static void
UpdateRangeTableOfViewParse(char *viewName, Query *viewParse)
{
    List *old_rt;
    List *new_rt;
    RangeTblEntry *rt_entry1, *rt_entry2;
    
    /*
     * first offset all var nodes by 2
     */
    OffsetVarNodes((Node*)viewParse->targetList, 2);
    OffsetVarNodes(viewParse->qual, 2);
    
    /*
     * find the old range table...
     */
    old_rt = viewParse->rtable;

    /*
     * create the 2 new range table entries and form the new
     * range table...
     * CURRENT first, then NEW....
     */
    rt_entry1 =
	addRangeTableEntry(NULL, (char*)viewName, "*CURRENT*",
						FALSE, FALSE, NULL);
    rt_entry2 =
	addRangeTableEntry(NULL, (char*)viewName, "*NEW*",
						FALSE, FALSE, NULL);
    new_rt = lcons(rt_entry2, old_rt);
    new_rt = lcons(rt_entry1, new_rt);
    
    /*
     * Now the tricky part....
     * Update the range table in place... Be careful here, or
     * hell breaks loooooooooooooOOOOOOOOOOOOOOOOOOSE!
     */
    viewParse->rtable = new_rt;
}

/*-------------------------------------------------------------------
 * DefineView
 *
 *	- takes a "viewname", "parsetree" pair and then
 *	1)	construct the "virtual" relation 
 *	2)	commit the command but NOT the transaction,
 *		so that the relation exists
 *		before the rules are defined.
 *	2)	define the "n" rules specified in the PRS2 paper
 *		over the "virtual" relation
 *-------------------------------------------------------------------
 */
void
DefineView(char *viewName, Query *viewParse)
{
    List *viewTlist;

    viewTlist = viewParse->targetList;
    
    /*
     * Create the "view" relation
     * NOTE: if it already exists, the xaxt will be aborted.
     */
    DefineVirtualRelation(viewName, viewTlist);
    
    /*
     * The relation we have just created is not visible
     * to any other commands running with the same transaction &
     * command id.
     * So, increment the command id counter (but do NOT pfree any
     * memory!!!!)
     */
    CommandCounterIncrement();
    
    /*
     * The range table of 'viewParse' does not contain entries
     * for the "CURRENT" and "NEW" relations.
     * So... add them!
     * NOTE: we make the update in place! After this call 'viewParse' 
     * will never be what it used to be...
     */
    UpdateRangeTableOfViewParse(viewName, viewParse);
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
    char* rname;
    
    /*
     * first remove all the "view" rules...
     * Currently we only have one!
     */
    rname = MakeRetrieveViewRuleName(viewName);
    RemoveRewriteRule(rname);

    /*
     * we don't really need that, but just in case...
     */
    CommandCounterIncrement();
    
    /*
     * now remove the relation.
     */
    heap_destroy(viewName);
    pfree(rname);
}
