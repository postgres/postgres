/*-------------------------------------------------------------------------
 *
 * planmain.c--
 *    Routines to plan a single query
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/optimizer/plan/planmain.c,v 1.2 1996/10/31 10:59:14 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"
#include "nodes/relation.h"

#include "optimizer/planmain.h"
#include "optimizer/internal.h"
#include "optimizer/paths.h"
#include "optimizer/clauses.h"
#include "optimizer/keys.h"
#include "optimizer/tlist.h"
#include "optimizer/xfunc.h"
#include "optimizer/cost.h"

#include "tcop/dest.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "nodes/memnodes.h"
#include "utils/mcxt.h"
#include "utils/lsyscache.h"

static Plan *subplanner(Query *root, List *flat_tlist, List *qual);
static Result *make_result(List *tlist, Node *resconstantqual, Plan *subplan);

static Plan *make_groupPlan(List *tlist, bool tuplePerGroup,
			    List *groupClause, Plan *subplan);

/*    
 * query_planner--
 *    Routine to create a query plan.  It does so by first creating a
 *    subplan for the topmost level of attributes in the query.  Then,
 *    it modifies all target list and qualifications to consider the next
 *    level of nesting and creates a plan for this modified query by
 *    recursively calling itself.  The two pieces are then merged together
 *    by creating a result node that indicates which attributes should
 *    be placed where and any relation level qualifications to be
 *    satisfied.
 *    
 *    command-type is the query command, e.g., retrieve, delete, etc.
 *    tlist is the target list of the query
 *    qual is the qualification of the query
 *    
 *    Returns a query plan.
 */
Plan *
query_planner(Query *root,
	      int command_type,
	      List *tlist,
	      List *qual)
{
    List 	*constant_qual = NIL;
    List	*flattened_tlist = NIL;
    List	*level_tlist = NIL;
    Plan	*subplan = (Plan*)NULL;
    Agg		*aggplan = NULL;
    
    /*
     * A command without a target list or qualification is an error,
     * except for "delete foo".
     */
    if (tlist==NIL && qual==NULL) {
	if (command_type == CMD_DELETE ||
	    /* Total hack here. I don't know how to handle
	       statements like notify in action bodies.
	       Notify doesn't return anything but
	       scans a system table. */
	    command_type == CMD_NOTIFY) {
	    return ((Plan*)make_seqscan(NIL,
					NIL,
					root->resultRelation,
					(Plan*)NULL));
	} else
	    return((Plan*)NULL);
    }
    
    /*
     * Pull out any non-variable qualifications so these can be put in
     * the topmost result node.  The opids for the remaining
     * qualifications will be changed to regprocs later.
     */
    qual = pull_constant_clauses(qual, &constant_qual);
    fix_opids(constant_qual);
    
    /*
     * Create a target list that consists solely of (resdom var) target
     * list entries, i.e., contains no arbitrary expressions.
     */
    flattened_tlist = flatten_tlist(tlist);
    if (flattened_tlist) {
	level_tlist = flattened_tlist;
    } else {
	/* from old code. the logic is beyond me. - ay 2/95 */
	level_tlist = tlist;
    }

    /*
     * Needs to add the group attribute(s) to the target list so that they
     * are available to either the Group node or the Agg node. (The target
     * list may not contain the group attribute(s).)
     */
    if (root->groupClause) {
	AddGroupAttrToTlist(level_tlist, root->groupClause);
    }
    
    if (root->qry_aggs) {
	aggplan = make_agg(tlist, root->qry_numAgg, root->qry_aggs);
	tlist = level_tlist;
    }

    /*
     * A query may have a non-variable target list and a non-variable
     * qualification only under certain conditions:
     *    - the query creates all-new tuples, or
     *    - the query is a replace (a scan must still be done in this case).
     */
    if (flattened_tlist==NULL && qual==NULL) {

	switch (command_type) {
	case CMD_SELECT: 
	case CMD_INSERT:
	    return ((Plan*)make_result(tlist,
				       (Node*)constant_qual,
				       (Plan*)NULL));
	    break;

	case CMD_DELETE:
	case CMD_UPDATE:
	    {
		SeqScan *scan = make_seqscan(tlist,
					     (List *)NULL,
					     root->resultRelation,
					     (Plan*)NULL);
		if (constant_qual!=NULL) {
		    return ((Plan*)make_result(tlist,
					       (Node*)constant_qual,
					       (Plan*)scan));
		} else {
		    return ((Plan*)scan);
		} 
	    }
	    break;
	       
	default:
	    return ((Plan*)NULL);
	}
    }

    /*
     * Find the subplan (access path) and destructively modify the
     * target list of the newly created subplan to contain the appropriate
     * join references.
     */
    subplan = subplanner(root, level_tlist, qual);
     
    set_tlist_references(subplan);

    /*
     * If we have a GROUP BY clause, insert a group node (with the appropriate
     * sort node.)
     */
    if (root->groupClause != NULL) {
	bool tuplePerGroup;

	/*
	 * decide whether how many tuples per group the Group node needs
	 * to return. (Needs only one tuple per group if no aggregate is
	 * present. Otherwise, need every tuple from the group to do the
	 * aggregation.)
	 */
	tuplePerGroup = (aggplan == NULL) ? FALSE : TRUE;
	
	subplan =
	    make_groupPlan(tlist, tuplePerGroup, root->groupClause, subplan);

	/* XXX fake it: this works for the Group node too! very very ugly,
	   please change me -ay 2/95 */
	set_agg_tlist_references((Agg*)subplan);
    }

    /*
     * If aggregate is present, insert the agg node 
     */
    if (aggplan != NULL) {
	aggplan->plan.lefttree = subplan;
	subplan = (Plan*)aggplan;

	/*
	 * set the varno/attno entries to the appropriate references to
	 * the result tuple of the subplans. (We need to set those in the
	 * array of aggreg's in the Agg node also. Even though they're 
	 * pointers, after a few dozen's of copying, they're not the same as
	 * those in the target list.)
	 */
	set_agg_tlist_references((Agg*)subplan);
	set_agg_agglist_references((Agg*)subplan);

	tlist = aggplan->plan.targetlist;
    }
    
    /*
     * Build a result node linking the plan if we have constant quals
     */
    if (constant_qual) {
	Plan *plan;

	plan = (Plan*)make_result(tlist,
				  (Node*)constant_qual,
				  subplan);
	/*
	 * Change all varno's of the Result's node target list.
	 */
	set_result_tlist_references((Result*)plan);

	return (plan);
    }

    /*
     * fix up the flattened target list of the plan root node so that
     * expressions are evaluated.  this forces expression evaluations
     * that may involve expensive function calls to be delayed to
     * the very last stage of query execution.  this could be bad.
     * but it is joey's responsibility to optimally push these
     * expressions down the plan tree.  -- Wei
     */
    subplan->targetlist = flatten_tlist_vars(tlist,
					     subplan->targetlist);

    /*
     * Destructively modify the query plan's targetlist to add fjoin
     * lists to flatten functions that return sets of base types
     */
    subplan->targetlist = generate_fjoin(subplan->targetlist);

    return (subplan);
}

/*    
 * subplanner
 *    
 *   Subplanner creates an entire plan consisting of joins and scans
 *   for processing a single level of attributes. 
 *    
 *   flat-tlist is the flattened target list
 *   qual is the qualification to be satisfied
 *    
 *   Returns a subplan.
 *    
 */
static Plan *
subplanner(Query *root,
	   List *flat_tlist,
	   List *qual)
{
    Rel *final_relation;
    List *final_relation_list;

    /* Initialize the targetlist and qualification, adding entries to
     * *query-relation-list* as relation references are found (e.g., in the
     *  qualification, the targetlist, etc.)
     */
    root->base_relation_list_ = NIL; 
    root->join_relation_list_ = NIL;
    initialize_base_rels_list(root, flat_tlist);
    initialize_base_rels_jinfo(root, qual);
    add_missing_vars_to_base_rels(root, flat_tlist);

    /* Find all possible scan and join paths.
     * Mark all the clauses and relations that can be processed using special
     * join methods, then do the exhaustive path search.
     */
    initialize_join_clause_info(root->base_relation_list_);
    final_relation_list = find_paths(root,
				     root->base_relation_list_);

    if (final_relation_list)
	final_relation = (Rel*)lfirst (final_relation_list);
    else
	final_relation = (Rel*)NIL;

#if 0 /* fix xfunc */
    /*
     * Perform Predicate Migration on each path, to optimize and correctly
     * assess the cost of each before choosing the cheapest one.
     *  						-- JMH, 11/16/92
     *
     * Needn't do so if the top rel is pruneable: that means there's no
     * expensive functions left to pull up.  -- JMH, 11/22/92
     */
    if (XfuncMode != XFUNC_OFF && XfuncMode != XFUNC_NOPM && 
	XfuncMode != XFUNC_NOPULL && !final_relation->pruneable)
	{
	    List *pathnode;
	    foreach(pathnode, final_relation->pathlist)
		{
		    if (xfunc_do_predmig((Path*)lfirst(pathnode)))
			set_cheapest(final_relation, final_relation->pathlist);
		}
	}
#endif
    
    /*
     * Determine the cheapest path and create a subplan corresponding to it.
     */
    if (final_relation) {
	return (create_plan ((Path*)final_relation->cheapestpath));
    }else {
	elog(NOTICE, "final relation is nil");
	return(create_plan ((Path*)NULL));
    }
    
}

/*****************************************************************************
 *
 *****************************************************************************/

static Result *
make_result(List *tlist,
	    Node *resconstantqual,
	    Plan *subplan)
{
    Result *node = makeNode(Result);
    Plan *plan = &node->plan;

    tlist = generate_fjoin(tlist);
    plan->cost = 0.0;
    plan->state = (EState *)NULL;
    plan->targetlist = tlist;
    plan->lefttree = subplan;
    plan->righttree = NULL;
    node->resconstantqual = resconstantqual;
    node->resstate = NULL;
    
    return(node);
} 

/*****************************************************************************
 *
 *****************************************************************************/

static Plan *
make_groupPlan(List *tlist,
	       bool tuplePerGroup,
	       List *groupClause,
	       Plan *subplan)
{
    List *sort_tlist;
    List *gl;
    int keyno;
    Sort *sortplan;
    Group *grpplan;
    int numCols;
    AttrNumber *grpColIdx;

    numCols = length(groupClause);
    grpColIdx = (AttrNumber *)palloc(sizeof(AttrNumber)*numCols);

    /*
     * first, make a sort node. Group node expects the tuples it gets
     * from the subplan is in the order as specified by the group columns.
     */
    keyno = 1;
    sort_tlist = new_unsorted_tlist(subplan->targetlist);

    {
	/* if this is a mergejoin node, varno could be OUTER/INNER */
	List *l;
	foreach(l, sort_tlist) {
	    TargetEntry *tle;
	    tle = lfirst(l);
	    ((Var*)tle->expr)->varno = 1;
	}
    }
    
    foreach (gl, groupClause) {
	GroupClause *grpcl = (GroupClause*)lfirst(gl);
	TargetEntry *tle;

	tle = match_varid(grpcl->grpAttr, sort_tlist);
	/*
	 * the parser should have checked to make sure the group attribute
	 * is valid but the optimizer might have screwed up and hence we
	 * check again.
	 */
	if (tle==NULL) {
	    elog(WARN, "group attribute disappeared from target list");
	}
	tle->resdom->reskey = keyno;
	tle->resdom->reskeyop = get_opcode(grpcl->grpOpoid);

	grpColIdx[keyno-1] = tle->resdom->resno;
	keyno++;
    }
    sortplan = make_sort(sort_tlist,
			 _TEMP_RELATION_ID_,
			 subplan,
			 numCols);
    sortplan->plan.cost = subplan->cost;	/* XXX assume no cost */

    /*
     * make the Group node
     */
    tlist = copyObject(tlist);	/* make a copy */
    grpplan = make_group(tlist, tuplePerGroup, numCols, grpColIdx, sortplan);
    
    return (Plan*)grpplan;
}
