/*-------------------------------------------------------------------------
 *
 * planner.c--
 *    The query optimizer external interface.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/optimizer/plan/planner.c,v 1.4 1997/09/05 19:32:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"
#include "nodes/relation.h"

#include "parser/catalog_utils.h"
#include "parser/parse_query.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "access/heapam.h"

#include "optimizer/internal.h"
#include "optimizer/planner.h"
#include "optimizer/plancat.h"   
#include "optimizer/prep.h"
#include "optimizer/planmain.h"
#include "optimizer/paths.h"
#include "optimizer/cost.h"

/* DATA STRUCTURE CREATION/MANIPULATION ROUTINES */
#include "nodes/relation.h"
#include "optimizer/clauseinfo.h"
#include "optimizer/joininfo.h"
#include "optimizer/keys.h"
#include "optimizer/ordering.h"
#include "optimizer/pathnode.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"

#include "executor/executor.h"

static Plan *make_sortplan(List *tlist, List *sortcls, Plan *plannode);
static Plan *init_query_planner(Query *parse);
static Existential *make_existential(Plan *left, Plan *right);

/*****************************************************************************
 *
 *     Query optimizer entry point
 *    
 *****************************************************************************/


/*    
 * planner--
 *    Main query optimizer routine.
 *    
 *    Invokes the planner on union queries if there are any left,
 *    recursing if necessary to get them all, then processes normal plans.
 *    
 * Returns a query plan.
 *    
 */
Plan* 
planner(Query *parse)
{
    List *tlist = parse->targetList;
    List *rangetable = parse->rtable;
    char* uniqueflag = parse->uniqueFlag;
    List *sortclause = parse->sortClause;
    Plan *special_plans = (Plan*)NULL;

    Plan *result_plan = (Plan*) NULL;

    int rt_index;

    /*
     * plan inheritance
     */
    rt_index = first_matching_rt_entry(rangetable, INHERITS_FLAG);
    if (rt_index != -1) {
	special_plans = (Plan *)plan_union_queries((Index)rt_index,
						   parse,
						   INHERITS_FLAG);
    }

    /*
     * plan archive queries
     */
    rt_index = first_matching_rt_entry(rangetable, ARCHIVE_FLAG);
    if (rt_index != -1) {
	special_plans = (Plan *)plan_union_queries((Index)rt_index,
						   parse,
						   ARCHIVE_FLAG);
    }

    if (special_plans)
      result_plan = special_plans;
    else
      result_plan = init_query_planner(parse); /* regular plans */
    
    /*
     *  For now, before we hand back the plan, check to see if there
     *  is a user-specified sort that needs to be done.  Eventually, this 
     *  will be moved into the guts of the planner s.t. user specified 
     *  sorts will be considered as part of the planning process. 
     *  Since we can only make use of user-specified sorts in
     *  special cases, we can do the optimization step later.
     */

    if (uniqueflag) {
      Plan *sortplan = make_sortplan(tlist, sortclause, result_plan);
					  
      return((Plan*)make_unique(tlist,sortplan,uniqueflag));
    } else {
      if (sortclause) 
	return(make_sortplan(tlist,sortclause,result_plan));
      else 
	return((Plan*)result_plan);
    }

}

/*
 * make_sortplan--
 *    Returns a sortplan which is basically a SORT node attached to the
 *    top of the plan returned from the planner.  It also adds the 
 *     cost of sorting into the plan.
 *      
 * sortkeys: ( resdom1 resdom2 resdom3 ...)
 * sortops:  (sortop1 sortop2 sortop3 ...)
 */
static Plan *
make_sortplan(List *tlist, List *sortcls, Plan *plannode)
{
    Plan *sortplan = (Plan*)NULL;
    List *temp_tlist = NIL;
    List *i = NIL;
    Resdom *resnode = (Resdom*)NULL;
    Resdom *resdom = (Resdom*)NULL;
    int keyno =1;

    /*  First make a copy of the tlist so that we don't corrupt the 
     *  the original .
     */
  
    temp_tlist = new_unsorted_tlist(tlist);
  
    foreach (i, sortcls) {
	SortClause *sortcl = (SortClause*)lfirst(i);

	resnode = sortcl->resdom;
	resdom = tlist_resdom(temp_tlist, resnode);

	/*    Order the resdom keys and replace the operator OID for each 
	 *    key with the regproc OID. 
	 */
	resdom->reskey = keyno;
	resdom->reskeyop = get_opcode(sortcl->opoid);
	keyno += 1;
    }

    sortplan = (Plan*)make_sort(temp_tlist,
				_TEMP_RELATION_ID_,
				(Plan*)plannode,
				length(sortcls));

    /*
     *  XXX Assuming that an internal sort has no. cost. 
     *      This is wrong, but given that at this point, we don't
     *      know the no. of tuples returned, etc, we can't do
     *      better than to add a constant cost.
     *      This will be fixed once we move the sort further into the planner,
     *      but for now ... functionality....
     */

    sortplan->cost = plannode->cost;
  
    return(sortplan);
}


/*    
 * init-query-planner--
 *    Deals with all non-union preprocessing, including existential
 *    qualifications and CNFifying the qualifications.
 *    
 * Returns a query plan.
 * MODIFIES: tlist,qual
 *    
 */
static Plan *
init_query_planner(Query *root)
{
    List *primary_qual;
    List *existential_qual;
    Existential *exist_plan;
    List *tlist = root->targetList;

    tlist = preprocess_targetlist(tlist,
				  root->commandType,
				  root->resultRelation,
				  root->rtable);

    primary_qual =
	preprocess_qualification((Expr*)root->qual,
				 tlist,
				 &existential_qual);

    if(existential_qual==NULL) {
	return(query_planner(root,
			     root->commandType,
			     tlist,
			     primary_qual));
    } else {
	int temp = root->commandType;
	Plan *existential_plan;

	root->commandType = CMD_SELECT;
	existential_plan = query_planner(root,
					 temp, 
					 NIL,
					 existential_qual);
     
	exist_plan = make_existential(existential_plan,
				      query_planner(root,
						    root->commandType,
						    tlist,
						    primary_qual));
	return((Plan*)exist_plan);
    }
}

/* 
 * make_existential--
 *    Instantiates an existential plan node and fills in 
 *    the left and right subtree slots.
 */
static Existential *
make_existential(Plan *left, Plan *right)
{
    Existential *node = makeNode(Existential);

    node->lefttree = left;
    node->righttree = left;
    return(node);
}

/*
 * pg_checkretval() -- check return value of a list of sql parse
 *			trees.
 *
 * The return value of a sql function is the value returned by
 * the final query in the function.  We do some ad-hoc define-time
 * type checking here to be sure that the user is returning the
 * type he claims.
 */
void
pg_checkretval(Oid rettype, QueryTreeList *queryTreeList)
{
    Query *parse;
    List *tlist;
    List *rt;
    int cmd;
    Type typ;
    Resdom *resnode;
    Relation reln;
    Oid relid;
    Oid tletype;
    int relnatts;
    int i;

    /* find the final query */
    parse = queryTreeList->qtrees[queryTreeList->len - 1];

    /*
     *  test 1:  if the last query is a utility invocation, then there
     *  had better not be a return value declared.
     */
    if (parse->commandType == CMD_UTILITY) {
	if (rettype == InvalidOid)
	    return;
	else
	    elog(WARN, "return type mismatch in function decl: final query is a catalog utility");
    }

    /* okay, it's an ordinary query */
    tlist = parse->targetList;
    rt = parse->rtable;
    cmd = parse->commandType;
    
    /*
     *  test 2:  if the function is declared to return no value, then the
     *  final query had better not be a retrieve.
     */
    if (rettype == InvalidOid) {
	if (cmd == CMD_SELECT)
	    elog(WARN,
		 "function declared with no return type, but final query is a retrieve");
	else
	    return;
    }

    /* by here, the function is declared to return some type */
    if ((typ = (Type)get_id_type(rettype)) == NULL)
	elog(WARN, "can't find return type %d for function\n", rettype);

    /*
     *  test 3:  if the function is declared to return a value, then the
     *  final query had better be a retrieve.
     */
    if (cmd != CMD_SELECT)
	elog(WARN, "function declared to return type %s, but final query is not a retrieve", tname(typ));

    /*
     *  test 4:  for base type returns, the target list should have exactly
     *  one entry, and its type should agree with what the user declared.
     */

    if (get_typrelid(typ) == InvalidOid) {
	if (exec_tlist_length(tlist) > 1)
	    elog(WARN, "function declared to return %s returns multiple values in final retrieve", tname(typ));

	resnode = (Resdom*) ((TargetEntry*)lfirst(tlist))->resdom;
	if (resnode->restype != rettype)
	    elog(WARN, "return type mismatch in function: declared to return %s, returns %s", tname(typ), tname(get_id_type(resnode->restype)));

	/* by here, base return types match */
	return;
    }

    /*
     *  If the target list is of length 1, and the type of the varnode
     *  in the target list is the same as the declared return type, this
     *  is okay.  This can happen, for example, where the body of the
     *  function is 'retrieve (x = func2())', where func2 has the same
     *  return type as the function that's calling it.
     */
    if (exec_tlist_length(tlist) == 1) {
	resnode = (Resdom*) ((TargetEntry*)lfirst(tlist))->resdom;
	if (resnode->restype == rettype)
	    return;
    }

    /*
     *  By here, the procedure returns a (set of) tuples.  This part of
     *  the typechecking is a hack.  We look up the relation that is
     *  the declared return type, and be sure that attributes 1 .. n
     *  in the target list match the declared types.
     */
    reln = heap_open(get_typrelid(typ));

    if (!RelationIsValid(reln))
	elog(WARN, "cannot open relation relid %d", get_typrelid(typ));

    relid = reln->rd_id;
    relnatts = reln->rd_rel->relnatts;

    if (exec_tlist_length(tlist) != relnatts)
	elog(WARN, "function declared to return type %s does not retrieve (%s.*)", tname(typ), tname(typ));

    /* expect attributes 1 .. n in order */
    for (i = 1; i <= relnatts; i++) {
	TargetEntry *tle = lfirst(tlist);
	Node *thenode = tle->expr;

	tlist = lnext(tlist);
	tletype = exprType(thenode);

#if 0 /* fix me */
	/* this is tedious */
	if (IsA(thenode,Var))
	    tletype = (Oid) ((Var*)thenode)->vartype;
	else if (IsA(thenode,Const))
	    tletype = (Oid) ((Const*)thenode)->consttype;
	else if (IsA(thenode,Param)) {
	    tletype = (Oid) ((Param*)thenode)->paramtype;
	else if (IsA(thenode,Expr)) {
	    tletype = Expr;
	} else if (IsA(thenode,LispList)) {
	    thenode = lfirst(thenode);
	    if (IsA(thenode,Oper))
		tletype = (Oid) get_opresulttype((Oper*)thenode);
	    else if (IsA(thenode,Func))
		tletype = (Oid) get_functype((Func*)thenode);
	    else
		elog(WARN, "function declared to return type %s does not retrieve (%s.all)", tname(typ), tname(typ));
#endif
/*
	} else
	    elog(WARN, "function declared to return type %s does not retrieve (%s.all)", tname(typ), tname(typ));
*/
	/* reach right in there, why don't you? */
	if (tletype != reln->rd_att->attrs[i-1]->atttypid)
	    elog(WARN, "function declared to return type %s does not retrieve (%s.all)", tname(typ), tname(typ));
    }

    heap_close(reln);

    /* success */
    return;
}
