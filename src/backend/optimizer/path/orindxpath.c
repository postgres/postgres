/*-------------------------------------------------------------------------
 *
 * orindxpath.c
 *	  Routines to find index paths that match a set of 'or' clauses
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/orindxpath.c,v 1.31 1999/07/27 03:51:02 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"




#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/internal.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"


static void best_or_subclause_indices(Query *root, RelOptInfo *rel,
									  List *subclauses, List *indices,
									  List **indexquals,
									  List **indexids,
									  Cost *cost, Cost *selec);
static void best_or_subclause_index(Query *root, RelOptInfo *rel,
									List *indexqual, List *indices,
									int *retIndexid,
									Cost *retCost, Cost *retSelec);


/*
 * create_or_index_paths
 *	  Creates index paths for indices that match 'or' clauses.
 *	  create_index_paths() must already have been called.
 *
 * 'rel' is the relation entry for which the paths are to be defined on
 * 'clauses' is the list of available restriction clause nodes
 *
 * Returns a list of index path nodes.
 *
 */
List *
create_or_index_paths(Query *root,
					  RelOptInfo *rel, List *clauses)
{
	List	   *path_list = NIL;
	List	   *clist;

	foreach(clist, clauses)
	{
		RestrictInfo *clausenode = (RestrictInfo *) lfirst(clist);

		/*
		 * Check to see if this clause is an 'or' clause, and, if so,
		 * whether or not each of the subclauses within the 'or' clause
		 * has been matched by an index.  The information used was
		 * saved by create_index_paths().
		 */
		if (restriction_is_or_clause(clausenode) &&
			clausenode->indexids)
		{
			bool		all_indexable = true;
			List	   *temp;

			foreach(temp, clausenode->indexids)
			{
				if (lfirst(temp) == NIL)
				{
					all_indexable = false;
					break;
				}
			}
			if (all_indexable)
			{
				/*
				 * OK, build an IndexPath for this OR clause, using the
				 * best available index for each subclause.
				 */
				IndexPath  *pathnode = makeNode(IndexPath);
				List	   *indexquals;
				List	   *indexids;
				Cost		cost;
				Cost		selec;

				best_or_subclause_indices(root,
										  rel,
										  clausenode->clause->args,
										  clausenode->indexids,
										  &indexquals,
										  &indexids,
										  &cost,
										  &selec);

				pathnode->path.pathtype = T_IndexScan;
				pathnode->path.parent = rel;
				pathnode->path.pathorder = makeNode(PathOrder);
				pathnode->path.pathorder->ordtype = SORTOP_ORDER;

				/*
				 * This is an IndexScan, but the overall result will consist
				 * of tuples extracted in multiple passes (one for each
				 * subclause of the OR), so the result cannot be claimed
				 * to have any particular ordering.
				 */
				pathnode->path.pathorder->ord.sortop = NULL;
				pathnode->path.pathkeys = NIL;

				pathnode->indexqual = indexquals;
				pathnode->indexid = indexids;
				pathnode->path.path_cost = cost;
				clausenode->selectivity = (Cost) selec;

				path_list = lappend(path_list, pathnode);
			}
		}
	}

	return path_list;
}

/*
 * best_or_subclause_indices
 *	  Determines the best index to be used in conjunction with each subclause
 *	  of an 'or' clause and the cost of scanning a relation using these
 *	  indices.	The cost is the sum of the individual index costs, since
 *	  the executor will perform a scan for each subclause of the 'or'.
 *
 * This routine also creates the indexquals and indexids lists that will
 * be needed by the executor.  The indexquals list has one entry for each
 * scan of the base rel, which is a sublist of indexqual conditions to
 * apply in that scan.  The implicit semantics are AND across each sublist
 * of quals, and OR across the toplevel list (note that the executor
 * takes care not to return any single tuple more than once).  The indexids
 * list gives the index to be used in each scan.
 *
 * 'rel' is the node of the relation on which the indexes are defined
 * 'subclauses' are the subclauses of the 'or' clause
 * 'indices' is a list of sublists of the index nodes that matched each
 *		subclause of the 'or' clause
 * '*indexquals' gets the constructed indexquals for the path (a list
 *		of sublists of clauses, one sublist per scan of the base rel)
 * '*indexids' gets a list of the index IDs for each scan of the rel
 * '*cost' gets the total cost of the path
 * '*selec' gets the total selectivity of the path.
 */
static void
best_or_subclause_indices(Query *root,
						  RelOptInfo *rel,
						  List *subclauses,
						  List *indices,
						  List **indexquals,	/* return value */
						  List **indexids,		/* return value */
						  Cost *cost,			/* return value */
						  Cost *selec)			/* return value */
{
	List	   *slist;

	*indexquals = NIL;
	*indexids = NIL;
	*cost = (Cost) 0.0;
	*selec = (Cost) 0.0;

	foreach(slist, subclauses)
	{
		Expr	   *subclause = lfirst(slist);
		List	   *indexqual;
		int			best_indexid;
		Cost		best_cost;
		Cost		best_selec;

		/* Convert this 'or' subclause to an indexqual list */
		indexqual = make_ands_implicit(subclause);
		/* expand special operators to indexquals the executor can handle */
		indexqual = expand_indexqual_conditions(indexqual);

		best_or_subclause_index(root, rel, indexqual, lfirst(indices),
								&best_indexid, &best_cost, &best_selec);

		*indexquals = lappend(*indexquals, indexqual);
		*indexids = lappendi(*indexids, best_indexid);
		*cost += best_cost;
		/* We approximate the selectivity as the sum of the clause
		 * selectivities (but not more than 1).
		 * XXX This is too pessimistic, isn't it?
		 */
		*selec += best_selec;
		if (*selec > (Cost) 1.0)
			*selec = (Cost) 1.0;

		indices = lnext(indices);
	}
}

/*
 * best_or_subclause_index
 *	  Determines which is the best index to be used with a subclause of
 *	  an 'or' clause by estimating the cost of using each index and selecting
 *	  the least expensive.
 *
 * 'rel' is the node of the relation on which the index is defined
 * 'indexqual' is the indexqual list derived from the subclause
 * 'indices' is a list of index nodes that match the subclause
 * '*retIndexid' gets the ID of the best index
 * '*retCost' gets the cost of a scan with that index
 * '*retSelec' gets the selectivity of that scan
 */
static void
best_or_subclause_index(Query *root,
						RelOptInfo *rel,
						List *indexqual,
						List *indices,
						int *retIndexid,		/* return value */
						Cost *retCost,	/* return value */
						Cost *retSelec) /* return value */
{
	bool		first_run = true;
	List	   *ilist;

	/* if we don't match anything, return zeros */
	*retIndexid = 0;
	*retCost = (Cost) 0.0;
	*retSelec = (Cost) 0.0;

	foreach(ilist, indices)
	{
		RelOptInfo *index = (RelOptInfo *) lfirst(ilist);
		Oid			indexid = (Oid) lfirsti(index->relids);
		Cost		subcost;
		float		npages;
		float		selec;

		index_selectivity(root,
						  lfirsti(rel->relids),
						  indexid,
						  indexqual,
						  &npages,
						  &selec);

		subcost = cost_index(indexid,
							 (int) npages,
							 (Cost) selec,
							 rel->pages,
							 rel->tuples,
							 index->pages,
							 index->tuples,
							 false);

		if (first_run || subcost < *retCost)
		{
			*retIndexid = indexid;
			*retCost = subcost;
			*retSelec = selec;
			first_run = false;
		}
	}

}
