/*-------------------------------------------------------------------------
 *
 * prepunion.c--
 *    Routines to plan archive, inheritance, union, and version queries
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/optimizer/prep/prepunion.c,v 1.3 1997/01/10 20:18:12 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include <sys/types.h>

#include "postgres.h"

#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"
#include "nodes/relation.h"

#include "parser/parse_query.h"
#include "parser/parsetree.h"

#include "utils/elog.h"
#include "utils/lsyscache.h"

#include "optimizer/internal.h"
#include "optimizer/prep.h"
#include "optimizer/plancat.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"

static List *plan_union_query(List *relids, Index rt_index,
		 RangeTblEntry *rt_entry, Query *parse, UnionFlag flag,
		 List **union_rtentriesPtr);
static RangeTblEntry *new_rangetable_entry(Oid new_relid,
					   RangeTblEntry *old_entry);
static Query *subst_rangetable(Query *root, Index index,
			       RangeTblEntry *new_entry);
static void fix_parsetree_attnums(Index rt_index, Oid old_relid,
				  Oid new_relid, Query *parsetree);
static Append *make_append(List *unionplans, Index rt_index,
			  List *union_rt_entries, List *tlist);


/*    
 * find-all-inheritors -
 *    	Returns a list of relids corresponding to relations that inherit
 *    	attributes from any relations listed in either of the argument relid
 *    	lists.
 */
List *
find_all_inheritors(List *unexamined_relids,
		    List *examined_relids)
{
    List *new_inheritors = NIL;
    List *new_examined_relids = NIL;
    List *new_unexamined_relids = NIL;
    
    /* Find all relations which inherit from members of
     * 'unexamined-relids' and store them in 'new-inheritors'.
     */
    List *rels = NIL;
    List *newrels = NIL;
    
    foreach(rels,unexamined_relids) {
	newrels = (List*)LispUnioni(find_inheritance_children(lfirsti(rels)),
				    newrels);
    }
    new_inheritors = newrels;
    
    new_examined_relids = (List*)LispUnioni(examined_relids,unexamined_relids);
    new_unexamined_relids = set_differencei(new_inheritors,
					    new_examined_relids);
    
    if (new_unexamined_relids==NULL) {
	return(new_examined_relids);
    } else {
	return (find_all_inheritors (new_unexamined_relids,
				     new_examined_relids));
    }
}

/*    
 * first-matching-rt-entry -
 *    	Given a rangetable, find the first rangetable entry that represents
 *    	the appropriate special case.
 *    
 *    	Returns a rangetable index.,  Returns -1 if no matches
 */
int
first_matching_rt_entry (List *rangetable, UnionFlag flag) 
{
    int count = 0;
    List *temp = NIL;

    foreach(temp, rangetable) {
	RangeTblEntry *rt_entry = lfirst(temp);
	
	switch(flag) {
	case INHERITS_FLAG:
	    if (rt_entry->inh)
		return count+1;
	    break;
	case ARCHIVE_FLAG:
	    if (rt_entry->archive)
		return count+1;
	    break;
	default:
	    break;
	}
	count++;
    }
     
    return(-1);
}


/*    
 * plan-union-queries--
 *    
 *    Plans the queries for a given parent relation.
 *    
 * Returns a list containing a list of plans and a list of rangetable
 * entries to be inserted into an APPEND node.
 * XXX - what exactly does this mean, look for make_append
 */
Append *
plan_union_queries(Index rt_index,
		   Query *parse,
		   UnionFlag flag)
{
    List *rangetable = parse->rtable;
    RangeTblEntry *rt_entry = rt_fetch(rt_index,rangetable);
    List *union_relids = NIL;
    List *union_plans = NIL;
    List *union_rt_entries = NIL;
    
    switch (flag) {
    case INHERITS_FLAG:
	union_relids = 
	    find_all_inheritors(lconsi(rt_entry->relid,
					NIL),
				 NIL);
	break;

#if 0	
    case UNION_FLAG:
	{
	    Index rt_index = 0;
	    union_plans = handleunion(root,rangetable,tlist,qual);
	    return (make_append (union_plans,
				 rt_index, rangetable,
				 ((Plan*)lfirst(union_plans))->targetlist ));
	}
	break;
#endif
	
    case VERSION_FLAG:
	union_relids = VersionGetParents(rt_entry->relid);
	break;
	
    case ARCHIVE_FLAG:
	union_relids = find_archive_rels(rt_entry->relid);
	break;

    default: 
	/* do nothing */
	break;
    }

    /*
     * Remove the flag for this relation, since we're about to handle it 
     * (do it before recursing!).
     * XXX destructive parse tree change
     */
    switch(flag) {
    case INHERITS_FLAG:
	rt_fetch(rt_index,rangetable)->inh = false;
	break;
    case ARCHIVE_FLAG:
	rt_fetch(rt_index,rangetable)->archive = false;
	break;
    default:
	break;
    }

    /* XXX - can't find any reason to sort union-relids
     * as paul did, so we're leaving it out for now
     * (maybe forever) - jeff & lp
     *
     * [maybe so. btw, jeff & lp did the lisp conversion, according to Paul.
     *  -- ay 10/94.]
     */
    union_plans = plan_union_query(union_relids, rt_index, rt_entry,
				   parse, flag, &union_rt_entries);

    return (make_append(union_plans,
			rt_index,
			union_rt_entries,
			((Plan*)lfirst(union_plans))->targetlist));
}


/*    
 * plan-union-query--
 *    Returns a list of plans for 'relids' and a list of range table entries
 *    in union_rtentries.
 */
static List *
plan_union_query(List *relids,
		 Index rt_index,
		 RangeTblEntry *rt_entry,
		 Query *root,
		 UnionFlag flag,
		 List **union_rtentriesPtr)
{
    List *i;
    List *union_plans = NIL;
    List *union_rtentries = NIL;

    foreach (i, relids) {
	int relid = lfirsti(i);
	RangeTblEntry *new_rt_entry = new_rangetable_entry(relid,
							   rt_entry);
	Query *new_root = subst_rangetable(root,
					   rt_index,
					   new_rt_entry);

	/* reset the uniqueflag and sortclause in parse tree root, so that
	 * sorting will only be done once after append
	 */
/*	new_root->uniqueFlag = false; */
	new_root->uniqueFlag = NULL; 
	new_root->sortClause = NULL;
	if (flag == ARCHIVE_FLAG) {
	    /*
	     * the entire union query uses the same (most recent) schema.
	     * to do otherwise would require either ragged tuples or careful
	     * archiving and interpretation of pg_attribute...
	     */
	} else {
	    fix_parsetree_attnums(rt_index, 
				  rt_entry->relid,
				  relid,
				  new_root);
	}

	union_plans = lappend(union_plans, planner(new_root));
	union_rtentries = lappend(union_rtentries, new_rt_entry);
    }

    *union_rtentriesPtr = union_rtentries;
    return(union_plans);
}

/*    
 * new-rangetable-entry -
 *    	Replaces the name and relid of 'old-entry' with the values for 
 *    	'new-relid'.
 *    
 *    	Returns a copy of 'old-entry' with the parameters substituted.
 */
static RangeTblEntry *
new_rangetable_entry(Oid new_relid, RangeTblEntry *old_entry)
{
    RangeTblEntry *new_entry = copyObject(old_entry);

    /* ??? someone tell me what the following is doing! - ay 11/94 */
    if (!strcmp(new_entry->refname, "*CURRENT*") ||
        !strcmp(new_entry->refname, "*NEW*"))
	new_entry->refname =  get_rel_name(new_relid);
    else
	new_entry->relname =  get_rel_name(new_relid);

    new_entry->relid = new_relid;
    return(new_entry);
}

/*    
 * subst-rangetable--
 *    Replaces the 'index'th rangetable entry in 'root' with 'new-entry'.   
 *    
 * Returns a new copy of 'root'.
 */
static Query *
subst_rangetable(Query *root, Index index, RangeTblEntry *new_entry)
{
    Query *new_root = copyObject(root);
    List *temp = NIL;
    int i = 0;

    for(temp = new_root->rtable,i =1; i < index; temp =lnext(temp),i++)
	;
    lfirst(temp) = new_entry;

    return (new_root);
}

static void
fix_parsetree_attnums_nodes(Index rt_index,
			    Oid old_relid,
			    Oid new_relid,
			    Node *node)
{
    if (node==NULL)
	return;
    
    switch(nodeTag(node)) {
    case T_TargetEntry:
	{
	    TargetEntry *tle = (TargetEntry *)node;
	    
	    fix_parsetree_attnums_nodes(rt_index, old_relid, new_relid,
					tle->expr);
	}
	break;
    case T_Expr:
	{
	    Expr *expr = (Expr *)node;
	    fix_parsetree_attnums_nodes(rt_index, old_relid, new_relid,
					(Node*)expr->args);
	}
	break;
    case T_Var:
	{
	    Var *var = (Var *)node;
	    Oid old_typeid, new_typeid;

/*	    old_typeid = RelationIdGetTypeId(old_relid);*/
/*	    new_typeid = RelationIdGetTypeId(new_relid);*/
	    old_typeid = old_relid;
	    new_typeid = new_relid;

	    if (var->varno == rt_index && var->varattno != 0) {
		var->varattno =
		    get_attnum(new_typeid,
			       get_attname(old_typeid, var->varattno));
	    }
	}
	break;
    case T_List:
	{
	    List *l;
	    foreach(l, (List*)node) {
		fix_parsetree_attnums_nodes(rt_index, old_relid, new_relid,
					    (Node*)lfirst(l));
	    }
	}
	break;
    default:
	break;
    }
}
    
/*    
 * fix-parsetree-attnums--
 *    Replaces attribute numbers from the relation represented by
 *    'old-relid' in 'parsetree' with the attribute numbers from
 *    'new-relid'.
 *    
 * Returns the destructively-modified parsetree.
 *    
 */
static void
fix_parsetree_attnums(Index rt_index,
		      Oid old_relid,
		      Oid new_relid,
		      Query *parsetree)
{
    if (old_relid == new_relid)
	return;

    fix_parsetree_attnums_nodes(rt_index, old_relid, new_relid,
				(Node*)parsetree->targetList);
    fix_parsetree_attnums_nodes(rt_index, old_relid, new_relid,
				parsetree->qual);
}

static Append *
make_append(List *unionplans,
	    Index rt_index,
	    List *union_rt_entries,
	    List *tlist)
{
    Append *node = makeNode(Append);

    node->unionplans = unionplans;
    node->unionrelid = rt_index;
    node->unionrtentries = union_rt_entries;
    node->plan.cost = 0.0;
    node->plan.state = (EState*)NULL;
    node->plan.targetlist = tlist;
    node->plan.qual = NIL;
    node->plan.lefttree = (Plan*)NULL;
    node->plan.righttree = (Plan*)NULL;

    return(node);
}
