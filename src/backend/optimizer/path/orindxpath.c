/*-------------------------------------------------------------------------
 *
 * orindxpath.c--
 *    Routines to find index paths that match a set of 'or' clauses
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/optimizer/path/orindxpath.c,v 1.1.1.1 1996/07/09 06:21:36 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/relation.h"
#include "nodes/primnodes.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"

#include "optimizer/internal.h"
#include "optimizer/clauses.h"
#include "optimizer/clauseinfo.h"
#include "optimizer/paths.h"
#include "optimizer/cost.h"
#include "optimizer/plancat.h"
#include "optimizer/xfunc.h"

#include "parser/parsetree.h"


static void best_or_subclause_indices(Query *root, Rel *rel, List *subclauses,
   List *indices, List *examined_indexids, Cost subcost, List *selectivities,
   List **indexids, Cost *cost, List **selecs);			      
static void best_or_subclause_index(Query *root, Rel *rel, Expr *subclause,
	List *indices, int *indexid, Cost *cost, Cost *selec);


/*    
 * create-or-index-paths--
 *    Creates index paths for indices that match 'or' clauses.
 *    
 * 'rel' is the relation entry for which the paths are to be defined on
 * 'clauses' is the list of available restriction clause nodes
 *    
 * Returns a list of these index path nodes.
 *    
 */
List *
create_or_index_paths(Query *root,
		      Rel *rel, List *clauses)
{
    List *t_list = NIL;
    
    if (clauses != NIL) {
	CInfo *clausenode = (CInfo *) (lfirst (clauses));
	
	/* Check to see if this clause is an 'or' clause, and, if so,
	 * whether or not each of the subclauses within the 'or' clause has
	 * been matched by an index (the 'Index field was set in
	 * (match_or)  if no index matches a given subclause, one of the
	 * lists of index nodes returned by (get_index) will be 'nil').
	 */
	if (valid_or_clause(clausenode) &&
	    clausenode->indexids) {
	    List *temp = NIL;
	    List *index_list = NIL;
	    bool index_flag = true;
	    
	    index_list = clausenode->indexids;
	    foreach(temp,index_list) {
		if (!temp) 
		    index_flag = false;
	    }
	    if (index_flag) {   /* used to be a lisp every function */
		IndexPath *pathnode = makeNode(IndexPath);
		List *indexids;
		Cost cost;
		List *selecs;

		best_or_subclause_indices(root, 
					  rel,
					  clausenode->clause->args,
					  clausenode->indexids,
					  NIL,
					  (Cost)0,
					  NIL,
					  &indexids,
					  &cost,
					  &selecs);
		
		pathnode->path.pathtype = T_IndexScan;
		pathnode->path.parent = rel;
		pathnode->indexqual =
		    lcons(clausenode,NIL);
		pathnode->indexid = indexids;
		pathnode->path.path_cost = cost;
		
		/* copy clauseinfo list into path for expensive 
		   function processing    -- JMH, 7/7/92 */
		pathnode->path.locclauseinfo =
		    set_difference(clauses,
				   copyObject((Node*)
					      rel->clauseinfo));
		
#if 0 /* fix xfunc */
		/* add in cost for expensive functions!  -- JMH, 7/7/92 */
		if (XfuncMode != XFUNC_OFF) {
		    ((Path*)pathnode)->path_cost +=
			xfunc_get_path_cost((Path)pathnode);
		}
#endif		
		clausenode->selectivity = (Cost)floatVal(selecs);
		t_list = 
		    lcons(pathnode,
			 create_or_index_paths(root, rel,lnext(clauses)));
	    } else {
		t_list = create_or_index_paths(root, rel,lnext(clauses));
	    } 
	}
    }

    return(t_list);
}

/*    
 * best-or-subclause-indices--
 *    Determines the best index to be used in conjunction with each subclause
 *    of an 'or' clause and the cost of scanning a relation using these
 *    indices.  The cost is the sum of the individual index costs.
 *    
 * 'rel' is the node of the relation on which the index is defined
 * 'subclauses' are the subclauses of the 'or' clause
 * 'indices' are those index nodes that matched subclauses of the 'or'
 * 	clause
 * 'examined-indexids' is a list of those index ids to be used with 
 * 	subclauses that have already been examined
 * 'subcost' is the cost of using the indices in 'examined-indexids'
 * 'selectivities' is a list of the selectivities of subclauses that
 * 	have already been examined
 *    
 * Returns a list of the indexids, cost, and selectivities of each
 * subclause, e.g., ((i1 i2 i3) cost (s1 s2 s3)), where 'i' is an OID,
 * 'cost' is a flonum, and 's' is a flonum.
 */
static void
best_or_subclause_indices(Query *root,
			  Rel *rel,
			  List *subclauses,
			  List *indices,
			  List *examined_indexids,
			  Cost subcost,
			  List *selectivities,
			  List **indexids,	/* return value */
			  Cost *cost,		/* return value */
			  List **selecs)	/* return value */
{
    if (subclauses==NIL) {
	*indexids = nreverse(examined_indexids);
	*cost = subcost;
	*selecs = nreverse(selectivities);
    } else {
	int best_indexid;
	Cost best_cost;
	Cost best_selec;
	
	best_or_subclause_index(root, rel, lfirst(subclauses), lfirst(indices),
				&best_indexid, &best_cost, &best_selec);
	
	best_or_subclause_indices(root,
				  rel,
				  lnext(subclauses),
				  lnext(indices),
				  lconsi(best_indexid, examined_indexids),
				  subcost + best_cost,
				  lcons(makeFloat(best_selec), selectivities),
				  indexids,
				  cost,
				  selecs);
    } 
    return;
} 

/*    
 * best-or-subclause-index--
 *    Determines which is the best index to be used with a subclause of
 *    an 'or' clause by estimating the cost of using each index and selecting
 *    the least expensive.
 *    
 * 'rel' is the node of the relation on which the index is defined
 * 'subclause' is the subclause
 * 'indices' is a list of index nodes that match the subclause
 *    
 * Returns a list (index-id index-subcost index-selectivity)
 * (a fixnum, a fixnum, and a flonum respectively).
 *    
 */
static void
best_or_subclause_index(Query *root,
			Rel *rel,
			Expr *subclause,
			List *indices,
			int *retIndexid,	/* return value */
			Cost *retCost,		/* return value */
			Cost *retSelec)		/* return value */
{
    if (indices != NIL) {
	Datum 	value;
	int 	flag = 0;
	Cost 	subcost;
	Rel	*index = (Rel *)lfirst (indices);
	AttrNumber attno = (get_leftop (subclause))->varattno ;
	Oid 	opno = ((Oper*)subclause->oper)->opno;
	bool 	constant_on_right = non_null((Expr*)get_rightop(subclause));
	float	npages, selec;
	int	subclause_indexid;
	Cost	subclause_cost;
	Cost	subclause_selec;
	
	if(constant_on_right) {
	    value = ((Const*)get_rightop (subclause))->constvalue;
	} else {
	    value = NameGetDatum("");
	} 
	if(constant_on_right) {
	    flag = (_SELEC_IS_CONSTANT_ ||_SELEC_CONSTANT_RIGHT_);
	} else {
	    flag = _SELEC_CONSTANT_RIGHT_;
	} 
	index_selectivity(lfirsti(index->relids),
			  index->classlist,
			  lconsi(opno,NIL),
			  getrelid(lfirsti(rel->relids),
				   root->rtable),
			  lconsi(attno,NIL),
			  lconsi(value,NIL),
			  lconsi(flag,NIL),
			  1,
			  &npages,
			  &selec);
	
	subcost = cost_index((Oid) lfirsti(index->relids),
			     (int)npages,
			     (Cost)selec,
			     rel->pages,
			     rel->tuples,
			     index->pages,
			     index->tuples,
			     false);
	best_or_subclause_index(root,
				rel,
				subclause,
				lnext(indices),
				&subclause_indexid,
				&subclause_cost,
				&subclause_selec);

	if (subclause_indexid==0 || subcost < subclause_cost) {
	    *retIndexid = lfirsti(index->relids);
	    *retCost = subcost;
	    *retSelec = selec;
	} else { 
	    *retIndexid = 0;
	    *retCost = 0.0;
	    *retSelec = 0.0;
	} 
    } 
    return;
}
