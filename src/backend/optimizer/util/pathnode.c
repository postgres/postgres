/*-------------------------------------------------------------------------
 *
 * pathnode.c
 *	  Routines to manipulate pathlists and create path nodes
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/pathnode.c,v 1.59 2000/02/07 04:41:01 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>

#include "postgres.h"

#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"


/*****************************************************************************
 *		MISC. PATH UTILITIES
 *****************************************************************************/

/*
 * path_is_cheaper
 *	  Returns t iff 'path1' is cheaper than 'path2'.
 *
 */
bool
path_is_cheaper(Path *path1, Path *path2)
{
	return (bool) (path1->path_cost < path2->path_cost);
}

/*
 * set_cheapest
 *	  Finds the minimum cost path from among a relation's paths.
 *
 * 'parent_rel' is the parent relation
 * 'pathlist' is a list of path nodes corresponding to 'parent_rel'
 *
 * Returns and sets the relation entry field with the pathnode that
 * is minimum.
 *
 */
Path *
set_cheapest(RelOptInfo *parent_rel, List *pathlist)
{
	List	   *p;
	Path	   *cheapest_so_far;

	Assert(IsA(parent_rel, RelOptInfo));
	Assert(pathlist != NIL);

	cheapest_so_far = (Path *) lfirst(pathlist);

	foreach(p, lnext(pathlist))
	{
		Path	   *path = (Path *) lfirst(p);

		if (path_is_cheaper(path, cheapest_so_far))
			cheapest_so_far = path;
	}

	parent_rel->cheapestpath = cheapest_so_far;

	return cheapest_so_far;
}

/*
 * add_pathlist
 *	  Consider each path given in new_paths, and add it to the parent rel's
 *	  pathlist if it seems worthy.
 */
void
add_pathlist(RelOptInfo *parent_rel, List *new_paths)
{
	List	   *p1;

	foreach(p1, new_paths)
	{
		Path	   *new_path = (Path *) lfirst(p1);

		add_path(parent_rel, new_path);
	}
}

/*
 * add_path
 *	  Consider a potential implementation path for the specified parent rel,
 *	  and add it to the rel's pathlist if it is worthy of consideration.
 *	  A path is worthy if it has either a better sort order (better pathkeys)
 *	  or cheaper cost than any of the existing old paths.
 *
 *	  Unless parent_rel->pruneable is false, we also remove from the rel's
 *	  pathlist any old paths that are dominated by new_path --- that is,
 *	  new_path is both cheaper and at least as well ordered.
 *
 * 'parent_rel' is the relation entry to which the path corresponds.
 * 'new_path' is a potential path for parent_rel.
 *
 * Returns nothing, but modifies parent_rel->pathlist.
 */
void
add_path(RelOptInfo *parent_rel, Path *new_path)
{
	bool		accept_new = true; /* unless we find a superior old path */
	List	   *p1_prev = NIL;
	List	   *p1;

	/*
	 * Loop to check proposed new path against old paths.  Note it is
	 * possible for more than one old path to be tossed out because
	 * new_path dominates it.
	 */
	foreach(p1, parent_rel->pathlist)
	{
		Path	   *old_path = (Path *) lfirst(p1);
		bool		remove_old = false;	/* unless new proves superior */

		switch (compare_pathkeys(new_path->pathkeys, old_path->pathkeys))
		{
			case PATHKEYS_EQUAL:
				if (new_path->path_cost < old_path->path_cost)
					remove_old = true; /* new dominates old */
				else
					accept_new = false;	/* old equals or dominates new */
				break;
			case PATHKEYS_BETTER1:
				if (new_path->path_cost <= old_path->path_cost)
					remove_old = true; /* new dominates old */
				break;
			case PATHKEYS_BETTER2:
				if (new_path->path_cost >= old_path->path_cost)
					accept_new = false;	/* old dominates new */
				break;
			case PATHKEYS_DIFFERENT:
				/* keep both paths, since they have different ordering */
				break;
		}

		/*
		 * Remove current element from pathlist if dominated by new,
		 * unless xfunc told us not to remove any paths.
		 */
		if (remove_old && parent_rel->pruneable)
		{
			if (p1_prev)
				lnext(p1_prev) = lnext(p1);
			else
				parent_rel->pathlist = lnext(p1);
		}
		else
			p1_prev = p1;

		/*
		 * If we found an old path that dominates new_path, we can quit
		 * scanning the pathlist; we will not add new_path, and we assume
		 * new_path cannot dominate any other elements of the pathlist.
		 */
		if (! accept_new)
			break;
	}

	if (accept_new)
	{
		/* Accept the path */
		parent_rel->pathlist = lcons(new_path, parent_rel->pathlist);
	}
}


/*****************************************************************************
 *		PATH NODE CREATION ROUTINES
 *****************************************************************************/

/*
 * create_seqscan_path
 *	  Creates a path corresponding to a sequential scan, returning the
 *	  pathnode.
 *
 */
Path *
create_seqscan_path(RelOptInfo *rel)
{
	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_SeqScan;
	pathnode->parent = rel;
	pathnode->pathkeys = NIL;	/* seqscan has unordered result */
	pathnode->path_cost = cost_seqscan(rel);

	return pathnode;
}

/*
 * create_index_path
 *	  Creates a path node for an index scan.
 *
 * 'rel' is the parent rel
 * 'index' is an index on 'rel'
 * 'restriction_clauses' is a list of RestrictInfo nodes
 *			to be used as index qual conditions in the scan.
 *
 * Returns the new path node.
 */
IndexPath  *
create_index_path(Query *root,
				  RelOptInfo *rel,
				  IndexOptInfo *index,
				  List *restriction_clauses)
{
	IndexPath  *pathnode = makeNode(IndexPath);
	List	   *indexquals;

	pathnode->path.pathtype = T_IndexScan;
	pathnode->path.parent = rel;
	pathnode->path.pathkeys = build_index_pathkeys(root, rel, index);

	indexquals = get_actual_clauses(restriction_clauses);
	/* expand special operators to indexquals the executor can handle */
	indexquals = expand_indexqual_conditions(indexquals);

	/*
	 * We are making a pathnode for a single-scan indexscan; therefore,
	 * both indexid and indexqual should be single-element lists.
	 */
	pathnode->indexid = lconsi(index->indexoid, NIL);
	pathnode->indexqual = lcons(indexquals, NIL);
	pathnode->joinrelids = NIL;	/* no join clauses here */

	pathnode->path.path_cost = cost_index(root, rel, index, indexquals,
										  false);

	return pathnode;
}

/*
 * create_tidscan_path
 *	  Creates a path corresponding to a tid_direct scan, returning the
 *	  pathnode.
 *
 */
TidPath *
create_tidscan_path(RelOptInfo *rel, List *tideval)
{
	TidPath	*pathnode = makeNode(TidPath);

	pathnode->path.pathtype = T_TidScan;
	pathnode->path.parent = rel;
	pathnode->path.pathkeys = NIL;
	pathnode->path.path_cost = cost_tidscan(rel, tideval);
	/* divide selectivity for each clause to get an equal selectivity
	 * as IndexScan does OK ? 
	*/
	pathnode->tideval = copyObject(tideval); /* is copy really necessary? */
	pathnode->unjoined_relids = NIL;

	return pathnode;
}

/*
 * create_nestloop_path
 *	  Creates a pathnode corresponding to a nestloop join between two
 *	  relations.
 *
 * 'joinrel' is the join relation.
 * 'outer_path' is the outer path
 * 'inner_path' is the inner path
 * 'restrict_clauses' are the RestrictInfo nodes to apply at the join
 * 'pathkeys' are the path keys of the new join path
 *
 * Returns the resulting path node.
 *
 */
NestPath   *
create_nestloop_path(RelOptInfo *joinrel,
					 Path *outer_path,
					 Path *inner_path,
					 List *restrict_clauses,
					 List *pathkeys)
{
	NestPath   *pathnode = makeNode(NestPath);

	pathnode->path.pathtype = T_NestLoop;
	pathnode->path.parent = joinrel;
	pathnode->outerjoinpath = outer_path;
	pathnode->innerjoinpath = inner_path;
	pathnode->joinrestrictinfo = restrict_clauses;
	pathnode->path.pathkeys = pathkeys;

	pathnode->path.path_cost = cost_nestloop(outer_path,
											 inner_path,
											 IsA(inner_path, IndexPath));

	return pathnode;
}

/*
 * create_mergejoin_path
 *	  Creates a pathnode corresponding to a mergejoin join between
 *	  two relations
 *
 * 'joinrel' is the join relation
 * 'outer_path' is the outer path
 * 'inner_path' is the inner path
 * 'restrict_clauses' are the RestrictInfo nodes to apply at the join
 * 'pathkeys' are the path keys of the new join path
 * 'mergeclauses' are the applicable join/restriction clauses
 * 'outersortkeys' are the sort varkeys for the outer relation
 * 'innersortkeys' are the sort varkeys for the inner relation
 *
 */
MergePath  *
create_mergejoin_path(RelOptInfo *joinrel,
					  Path *outer_path,
					  Path *inner_path,
					  List *restrict_clauses,
					  List *pathkeys,
					  List *mergeclauses,
					  List *outersortkeys,
					  List *innersortkeys)
{
	MergePath  *pathnode = makeNode(MergePath);

	/*
	 * If the given paths are already well enough ordered, we can skip
	 * doing an explicit sort.
	 */
	if (outersortkeys &&
		pathkeys_contained_in(outersortkeys, outer_path->pathkeys))
		outersortkeys = NIL;
	if (innersortkeys &&
		pathkeys_contained_in(innersortkeys, inner_path->pathkeys))
		innersortkeys = NIL;

	pathnode->jpath.path.pathtype = T_MergeJoin;
	pathnode->jpath.path.parent = joinrel;
	pathnode->jpath.outerjoinpath = outer_path;
	pathnode->jpath.innerjoinpath = inner_path;
	pathnode->jpath.joinrestrictinfo = restrict_clauses;
	pathnode->jpath.path.pathkeys = pathkeys;
	pathnode->path_mergeclauses = mergeclauses;
	pathnode->outersortkeys = outersortkeys;
	pathnode->innersortkeys = innersortkeys;
	pathnode->jpath.path.path_cost = cost_mergejoin(outer_path,
													inner_path,
													outersortkeys,
													innersortkeys);

	return pathnode;
}

/*
 * create_hashjoin_path
 *	  Creates a pathnode corresponding to a hash join between two relations.
 *
 * 'joinrel' is the join relation
 * 'outer_path' is the cheapest outer path
 * 'inner_path' is the cheapest inner path
 * 'restrict_clauses' are the RestrictInfo nodes to apply at the join
 * 'hashclauses' is a list of the hash join clause (always a 1-element list)
 * 'innerdisbursion' is an estimate of the disbursion of the inner hash key
 *
 */
HashPath *
create_hashjoin_path(RelOptInfo *joinrel,
					 Path *outer_path,
					 Path *inner_path,
					 List *restrict_clauses,
					 List *hashclauses,
					 Selectivity innerdisbursion)
{
	HashPath   *pathnode = makeNode(HashPath);

	pathnode->jpath.path.pathtype = T_HashJoin;
	pathnode->jpath.path.parent = joinrel;
	pathnode->jpath.outerjoinpath = outer_path;
	pathnode->jpath.innerjoinpath = inner_path;
	pathnode->jpath.joinrestrictinfo = restrict_clauses;
	/* A hashjoin never has pathkeys, since its ordering is unpredictable */
	pathnode->jpath.path.pathkeys = NIL;
	pathnode->path_hashclauses = hashclauses;
	pathnode->jpath.path.path_cost = cost_hashjoin(outer_path,
												   inner_path,
												   innerdisbursion);

	return pathnode;
}
