/*-------------------------------------------------------------------------
 *
 * rewriteDefine.c--
 *    routines for defining a rewrite rule
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/rewrite/rewriteDefine.c,v 1.2 1996/11/10 03:01:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>
#include "postgres.h"

#include "utils/rel.h"			/* for Relation stuff */
#include "access/heapam.h"		/* access methods like amopenr */
#include "utils/builtins.h"
#include "utils/elog.h"			/* for elog */
#include "utils/palloc.h"
#include "utils/lsyscache.h"		/* for get_typlen */
#include "nodes/pg_list.h"		/* for Lisp support */
#include "nodes/parsenodes.h"
#include "parser/catalog_utils.h"
#include "rewrite/locks.h"			
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteRemove.h"
#include "rewrite/rewriteSupport.h"
#include "tcop/tcopprot.h"

Oid LastOidProcessed = InvalidOid;

/*
 * This is too small for many rule plans, but it'll have to do for now.
 * Rule plans, etc will eventually have to be large objects.
 * 
 * should this be smaller?
 */
#define RULE_PLAN_SIZE 8192 

static void
strcpyq(char *dest, char *source)
{
    char *current=source,*destp= dest;	
    
    for(current=source; *current; current++) {
	if (*current == '\"')  {
	    *destp = '\\';
	    destp++;
	}
	*destp = *current;
	destp++;
    }
    *destp = '\0';
}

/*
 * InsertRule -
 *    takes the arguments and inserts them as attributes into the system 
 *    relation "pg_rewrite"
 *
 *	MODS :	changes the value of LastOidProcessed as a side
 *		effect of inserting the rule tuple
 *
 *	ARGS :	rulname		-	name of the rule
 *		evtype		-	one of RETRIEVE,REPLACE,DELETE,APPEND
 *		evobj		-	name of relation
 *		evslot		-	comma delimited list of slots
 *					if null => multi-attr rule
 *		evinstead	-	is an instead rule
 *		actiontree	-	parsetree(s) of rule action
 */
static Oid
InsertRule(char *rulname,
	   int evtype,
	   char *evobj,
	   char *evslot,
	   char *evqual,
	   bool evinstead,
	   char *actiontree)
{
    static char	rulebuf[RULE_PLAN_SIZE];
    static char actionbuf[RULE_PLAN_SIZE];
    static char qualbuf[RULE_PLAN_SIZE];
    Oid eventrel_oid = InvalidOid;
    AttrNumber evslot_index = InvalidAttrNumber;
    Relation eventrel = NULL;
    char *is_instead = "f";
    extern void eval_as_new_xact();
    char *template;
    
    eventrel = heap_openr(evobj);
    if (eventrel == NULL) {
	elog(WARN, "rules cannot be defined on relations not in schema");
    }
    eventrel_oid = RelationGetRelationId(eventrel);
    
    /*
     * if the slotname is null, we know that this is a multi-attr
     * rule
     */
    if (evslot == NULL)
	evslot_index = -1;
    else
	evslot_index = varattno(eventrel, (char*)evslot);
    heap_close(eventrel);

    if (evinstead)
	is_instead = "t";

    if (evqual == NULL)
	evqual = "nil";

    if (IsDefinedRewriteRule(rulname)) 
	elog(WARN, "Attempt to insert rule '%s' failed: already exists",
	     rulname);
    strcpyq(actionbuf,actiontree);	
    strcpyq(qualbuf, evqual);

    template = "INSERT INTO pg_rewrite \
(rulename, ev_type, ev_class, ev_attr, action, ev_qual, is_instead) VALUES \
('%s', %d::char, %d::oid, %d::int2, '%s'::text, '%s'::text, \
 '%s'::bool);";
    if (strlen(template) + strlen(rulname) + strlen(actionbuf) +
	strlen(qualbuf) + 20 /* fudge fac */ >  RULE_PLAN_SIZE) {
	elog(WARN, "DefineQueryRewrite: rule plan string too big.");
    }
    sprintf(rulebuf, template,
	    rulname, evtype, eventrel_oid, evslot_index, actionbuf,
	    qualbuf, is_instead);

    pg_eval(rulebuf, (char **) NULL, (Oid *) NULL, 0);
    
    return (LastOidProcessed);
}

/*
 *	for now, event_object must be a single attribute
 */
static void
ValidateRule(int event_type,
	     char *eobj_string,
	     char *eslot_string,
	     Node *event_qual,
	     List **action,
	     int is_instead,
	     Oid event_attype)
{
    if (((event_type == CMD_INSERT) || (event_type == CMD_DELETE)) &&
	eslot_string) {
	elog(WARN,
	     "rules not allowed for insert or delete events to an attribute");
    }

    if (event_qual && !*action && is_instead)
	elog(WARN,
	     "event_quals on 'instead nothing' rules not currently supported");

#if 0    
    /* on retrieve to class.attribute do instead nothing is converted
     * to 'on retrieve to class.attribute do instead
     *        retrieve (attribute = NULL)'
     * --- this is also a terrible hack that works well -- glass*/
    if (is_instead && !*action && eslot_string && event_type == CMD_SELECT) {
	char *temp_buffer = (char *) palloc(strlen(template)+80);
	sprintf(temp_buffer, template, event_attype,
		get_typlen(event_attype), eslot_string,
		event_attype);

	*action = (List*) stringToNode(temp_buffer);

	pfree(temp_buffer);
    }
#endif    
}

void
DefineQueryRewrite(RuleStmt *stmt)
{
    CmdType event_type	= stmt->event;
    Attr *event_obj	= stmt->object;
    Node *event_qual	= stmt->whereClause;
    bool is_instead	= stmt->instead;
    List *action	= stmt->actions;
    Relation event_relation = NULL ;
    Oid ruleId;
    Oid ev_relid	= 0;
    char *eslot_string	= NULL;
    int event_attno 	= 0;
    Oid event_attype	= 0;
    char *actionP, *event_qualP;
    
    extern Oid att_typeid();
    
    if (event_obj->attrs)
	eslot_string = strVal(lfirst(event_obj->attrs));
    else
	eslot_string = NULL;
    
    event_relation = heap_openr(event_obj->relname);
    if ( event_relation == NULL ) {
	elog(WARN, "virtual relations not supported yet");
    }
    ev_relid = RelationGetRelationId(event_relation);
    
    if (eslot_string == NULL) {
	event_attno = -1;
	event_attype = -1; /* XXX - don't care */
    } else {
	event_attno = varattno(event_relation, eslot_string);
	event_attype = att_typeid(event_relation,event_attno);
    }
    heap_close(event_relation);

    /* fix bug about instead nothing */
    ValidateRule(event_type, event_obj->relname,
		 eslot_string, event_qual, &action,
		 is_instead,event_attype);

    if (action == NULL) {
	if (!is_instead) return;	/* doesn't do anything */
	
	event_qualP = nodeToString(event_qual);
	
	ruleId = InsertRule(stmt->rulename,
			    event_type,
			    event_obj->relname,
			    eslot_string,
			    event_qualP,
			    true,
			    "nil");
	prs2_addToRelation(ev_relid, ruleId, event_type, event_attno, TRUE,
			   event_qual, NIL);

    } else {
	event_qualP = nodeToString(event_qual);
	actionP = nodeToString(action);

	ruleId = InsertRule(stmt->rulename,
			    event_type,
			    event_obj->relname,
			    eslot_string,
			    event_qualP,
			    is_instead,
			    actionP);
	
	/* what is the max size of type text? XXX -- glass */
	if (length(action) > 15 )
	    elog(WARN,"max # of actions exceeded");
	prs2_addToRelation(ev_relid, ruleId, event_type, event_attno,
			   is_instead, event_qual, action);
    }
}

