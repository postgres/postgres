/*-------------------------------------------------------------------------
 *
 * pathnode.c
 *	  Routines to manipulate pathlists and create path nodes
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/pathnode.c,v 1.54 1999/08/16 02:17:58 tgl Exp $
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
	Cost		cost1 = path1->path_cost;
	Cost		cost2 = path2->path_cost;

	return (bool) (cost1 < cost2);
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

	Assert(pathlist != NIL);
	Assert(IsA(parent_rel, RelOptInfo));

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
 *	  Construct an output path list by adding to old_paths each path in
 *	  new_paths that is worth considering --- that is, it has either a
 *	  better sort order (better pathkeys) or cheaper cost than any of the
 *	  existing old paths.
 *
 *	  Unless parent_rel->pruneable is false, we also remove from the output
 *	  pathlist any old paths that are dominated by added path(s) --- that is,
 *	  some new path is both cheaper and at least as well ordered.
 *
 *	  Note: the list old_paths is destructively modified, and in fact is
 *	  turned into the output list.
 *
 * 'parent_rel' is the relation entry to which these paths correspond.
 * 'old_paths' is the list of previously accepted paths for parent_rel.
 * 'new_paths' is a list of potential new paths.
 *
 * Returns the updated list of interesting pathnodes.
 */
List *
add_pathlist(RelOptInfo *parent_rel, List *old_paths, List *new_paths)
{
	List	   *p1;

	foreach(p1, new_paths)
	{
		Path	   *new_path = (Path *) lfirst(p1);
		bool		accept_new = true; /* unless we find a superior old path */
		List	   *p2_prev = NIL;
		List	   *p2;

		/*
		 * Loop to check proposed new path against old paths.  Note it is
		 * possible for more than one old path to be tossed out because
		 * new_path dominates it.
		 */
		foreach(p2, old_paths)
		{
			Path	   *old_path = (Path *) lfirst(p2);
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
			 * Remove current element from old_list if dominated by new,
			 * unless xfunc told us not to remove any paths.
			 */
			if (remove_old && parent_rel->pruneable)
			{
				if (p2_prev)
					lnext(p2_prev) = lnext(p2);
				else
					old_paths = lnext(p2);
			}
			else
				p2_prev = p2;

			/*
			 * If we found an old path that dominates new_path, we can quit
			 * scanning old_paths; we will not add new_path, and we assume
			 * new_path cannot dominate any other elements of old_paths.
			 */
			if (! accept_new)
				break;
		}

		if (accept_new)
		{
			/* Accept the path.  Note that it will now be eligible to be
			 * compared against the additional elements of new_paths...
			 */
			new_path->parent = parent_rel; /* not redundant, see prune.c */
			old_paths = lcons(new_path, old_paths);
		}
	}

	return old_paths;
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
	int			relid = 0;

	pathnode->pathtype = T_SeqScan;
	pathnode->parent = rel;
	pathnode->path_cost = 0.0;
	pathnode->pathkeys = NIL;	/* seqscan has unordered result */

	if (rel->relids != NIL)		/* can this happen? */
		relid = lfirsti(rel->relids);

	pathnode->path_cost = cost_seqscan(relid,
									   rel->pages, rel->tuples);

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
				  RelOptInfo *index,
				  List *restriction_clauses)
{
	IndexPath  *pathnode = makeNode(IndexPath);

	pathnode->path.pathtype = T_IndexScan;
	pathnode->path.parent = rel;
	pathnode->path.pathkeys = build_index_pathkeys(root, rel, index);

	/*
	 * Note that we are making a pathnode for a single-scan indexscan;
	 * therefore, both indexid and indexqual should be single-element
	 * lists.  We initialize indexqual to contain one empty sublist,
	 * representing a single index traversal with no index restriction
	 * conditions.  If we do have restriction conditions to use, they
	 * will get inserted below.
	 */
	Assert(length(index->relids) == 1);
	pathnode->indexid = index->relids;
	pathnode->indexqual = lcons(NIL, NIL);
	pathnode->joinrelids = NIL;	/* no join clauses here */

	if (restriction_clauses == NIL)
	{
		/*
		 * We have no restriction clauses, so compute scan cost using
		 * selectivity of 1.0.
		 */
		pathnode->path.path_cost = cost_index(lfirsti(index->relids),
											  index->pages,
											  1.0,
											  rel->pages,
											  rel->tuples,
											  index->pages,
											  index->tuples,
											  false);
	}
	else
	{
		/*
		 * Compute scan cost for the case when 'index' is used with
		 * restriction clause(s).  Also, place indexqual in path node.
		 */
		List	   *indexquals;
		float		npages;
		float		selec;
		Cost		clausesel;

		indexquals = get_actual_clauses(restriction_clauses);
		/* expand special operators to indexquals the executor can handle */
		indexquals = expand_indexqual_conditions(indexquals);

		/* Insert qual list into 1st sublist of pathnode->indexqual;
		 * we already made the cons cell above, no point in wasting it...
		 */
		lfirst(pathnode->indexqual) = indexquals;

		index_selectivity(root,
						  lfirsti(rel->relids),
						  lfirsti(index->relids),
						  indexquals,
						  &npages,
						  &selec);

		pathnode->path.path_cost = cost_index(lfirsti(index->relids),
											  (int) npages,
											  selec,
											  rel->pages,
											  rel->tuples,
											  index->pages,
											  index->tuples,
											  false);

		/*
		 * Set selectivities of clauses used with index to the selectivity
		 * of this index, subdividing the selectivity equally over each of
		 * the clauses.  To the extent that index_selectivity() can make a
		 * better estimate of the joint selectivity of these clauses than
		 * the product of individual estimates from compute_clause_selec()
		 * would be, this should give us a more accurate estimate of the
		 * total selectivity of all the clauses.
		 *
		 * XXX If there is more than one useful index for this rel, and the
		 * indexes can be used with different but overlapping groups of
		 * restriction clauses, we may end up with too optimistic an estimate,
		 * since set_clause_selectivities() will save the minimum of the
		 * per-clause selectivity estimated with each index.  But that should
		 * be fairly unlikely for typical index usage.
		 */
		clausesel = pow(selec, 1.0 / (double) length(restriction_clauses));
		set_clause_selectivities(restriction_clauses, clausesel);
	}

	return pathnode;
}

/*
 * create_nestloop_path
 *	  Creates a pathnode corresponding to a nestloop join between two
 *	  relations.
 *
 * 'joinrel' is the join relation.
 * 'outer_rel' is the outer join relation
 * 'outer_path' is the outer path
 * 'inner_path' is the inner path
 * 'pathkeys' are the path keys of the new join path
 *
 * Returns the resulting path node.
 *
 */
NestPath   *
create_nestloop_path(RelOptInfo *joinrel,
					 RelOptInfo *outer_rel,
					 Path *outer_path,
					 Path *inner_path,
					 List *pathkeys)
{
	NestPath   *pathnode = makeNode(NestPath);

	pathnode->path.pathtype = T_NestLoop;
	pathnode->path.parent = joinrel;
	pathnode->outerjoinpath = outer_path;
	pathnode->innerjoinpath = inner_path;
	pathnode->pathinfo = joinrel->restrictinfo;
	pathnode->path.pathkeys = pathkeys;

	pathnode->path.path_cost = cost_nestloop(outer_path->path_cost,
											 inner_path->path_cost,
											 outer_rel->size,
											 inner_path->parent->size,
											 page_size(outer_rel->size,
													   outer_rel->width),
											 IsA(inner_path, IndexPath));

	return pathnode;
}

/*
 * create_mergejoin_path
 *	  Creates a pathnode corresponding to a mergejoin join between
 *	  two relations
 *
 * 'joinrel' is the join relation
 * 'outersize' is the number of tuples in the outer relation
 * 'innersize' is the number of tuples in the inner relation
 * 'outerwidth' is the number of bytes per tuple in the outer relation
 * 'innerwidth' is the number of bytes per tuple in the inner relation
 * 'outer_path' is the outer path
 * 'inner_path' is the inner path
 * 'pathkeys' are the path keys of the new join path
 * 'mergeclauses' are the applicable join/restriction clauses
 * 'outersortkeys' are the sort varkeys for the outer relation
 * 'innersortkeys' are the sort varkeys for the inner relation
 *
 */
MergePath  *
create_mergejoin_path(RelOptInfo *joinrel,
					  int outersize,
					  int innersize,
					  int outerwidth,
					  int innerwidth,
					  Path *outer_path,
					  Path *inner_path,
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
	pathnode->jpath.pathinfo = joinrel->restrictinfo;
	pathnode->jpath.path.pathkeys = pathkeys;
	pathnode->path_mergeclauses = mergeclauses;
	pathnode->outersortkeys = outersortkeys;
	pathnode->innersortkeys = innersortkeys;
	pathnode->jpath.path.path_cost = cost_mergejoin(outer_path->path_cost,
													inner_path->path_cost,
													outersortkeys,
													innersortkeys,
													outersize,
													innersize,
													outerwidth,
													innerwidth);

	return pathnode;
}

/*
 * create_hashjoin_path
 *	  Creates a pathnode corresponding to a hash join between two relations.
 *
 * 'joinrel' is the join relation
 * 'outersize' is the number of tuples in the outer relation
 * 'innersize' is the number of tuples in the inner relation
 * 'outerwidth' is the number of bytes per tuple in the outer relation
 * 'innerwidth' is the number of bytes per tuple in the inner relation
 * 'outer_path' is the cheapest outer path
 * 'inner_path' is the cheapest inner path
 * 'hashclauses' is a list of the hash join clause (always a 1-element list)
 * 'innerdisbursion' is an estimate of the disbursion of the inner hash key
 *
 */
HashPath *
create_hashjoin_path(RelOptInfo *joinrel,
					 int outersize,
					 int innersize,
					 int outerwidth,
					 int innerwidth,
					 Path *outer_path,
					 Path *inner_path,
					 List *hashclauses,
					 Cost innerdisbursion)
{
	HashPath   *pathnode = makeNode(HashPath);

	pathnode->jpath.path.pathtype = T_HashJoin;
	pathnode->jpath.path.parent = joinrel;
	pathnode->jpath.outerjoinpath = outer_path;
	pathnode->jpath.innerjoinpath = inner_path;
	pathnode->jpath.pathinfo = joinrel->restrictinfo;
	/* A hashjoin never has pathkeys, since its ordering is unpredictable */
	pathnode->jpath.path.pathkeys = NIL;
	pathnode->path_hashclauses = hashclauses;
	pathnode->jpath.path.path_cost = cost_hashjoin(outer_path->path_cost,
												   inner_path->path_cost,
												   outersize, innersize,
												   outerwidth, innerwidth,
												   innerdisbursion);

	return pathnode;
}
