/*-------------------------------------------------------------------------
 *
 * pathnode.c
 *	  Routines to manipulate pathlists and create path nodes
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/pathnode.c,v 1.53 1999/08/06 04:00:17 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>

#include "postgres.h"



#include "optimizer/cost.h"
#include "optimizer/keys.h"
#include "optimizer/ordering.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"

static Path *better_path(Path *new_path, List *unique_paths, bool *is_new);


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
 *	  For each path in the list 'new_paths', add to the list 'unique_paths'
 *	  only those paths that are unique (i.e., unique ordering and ordering
 *	  keys).  Should a conflict arise, the more expensive path is thrown out,
 *	  thereby pruning the plan space.  But we don't prune if xfunc
 *	  told us not to.
 *
 * 'parent_rel' is the relation entry to which these paths correspond.
 *
 * Returns the list of unique pathnodes.
 *
 */
List *
add_pathlist(RelOptInfo *parent_rel, List *unique_paths, List *new_paths)
{
	List	   *p1;

	foreach(p1, new_paths)
	{
		Path	   *new_path = (Path *) lfirst(p1);
		Path	   *old_path;
		bool		is_new;

		/* Is this new path already in unique_paths? */
		if (member(new_path, unique_paths))
			continue;

		/* Find best matching path */
		old_path = better_path(new_path, unique_paths, &is_new);

		if (is_new)
		{
			/* This is a brand new path.  */
			new_path->parent = parent_rel;
			unique_paths = lcons(new_path, unique_paths);
		}
		else if (old_path == NULL)
		{
			;					/* do nothing if path is not cheaper */
		}
		else if (old_path != NULL)
		{						/* (IsA(old_path,Path)) { */
			new_path->parent = parent_rel;
			if (!parent_rel->pruneable)
				unique_paths = lcons(new_path, unique_paths);
			else
				unique_paths = lcons(new_path,
									 LispRemove(old_path, unique_paths));
		}
	}
	return unique_paths;
}

/*
 * better_path
 *	  Determines whether 'new_path' has the same ordering and keys as some
 *	  path in the list 'unique_paths'.	If there is a redundant path,
 *	  eliminate the more expensive path.
 *
 * Returns:
 *	  The old path - if 'new_path' matches some path in 'unique_paths' and is
 *				cheaper
 *	  nil - if 'new_path' matches but isn't cheaper
 *	  t - if there is no path in the list with the same ordering and keys
 *
 */
static Path *
better_path(Path *new_path, List *unique_paths, bool *is_new)
{
	Path	   *path = (Path *) NULL;
	List	   *temp = NIL;
	int			better_key;
	int			better_sort;

#ifdef OPTDUP_DEBUG
	printf("better_path entry\n");
	printf("new\n");
	pprint(new_path);
	printf("unique_paths\n");
	pprint(unique_paths);
#endif

	foreach(temp, unique_paths)
	{
		path = (Path *) lfirst(temp);

#ifdef OPTDUP_DEBUG
		if (!pathkeys_match(new_path->pathkeys, path->pathkeys, &better_key) ||
			better_key != 0)
		{
			printf("betterkey = %d\n", better_key);
			printf("newpath\n");
			pprint(new_path->pathkeys);
			printf("oldpath\n");
			pprint(path->pathkeys);
			if (path->pathkeys && new_path->pathkeys &&
				length(lfirst(path->pathkeys)) >= 2		/* &&
														 * length(lfirst(path->pa
														 * thkeys)) <
														 * length(lfirst(new_path
					->pathkeys)) */ )
				sleep(0);		/* set breakpoint here */
		}
		if (!pathorder_match(new_path->pathorder, path->pathorder,
							 &better_sort) ||
			better_sort != 0)
		{
			printf("neword\n");
			pprint(new_path->pathorder);
			printf("oldord\n");
			pprint(path->pathorder);
		}
#endif

		if (pathkeys_match(new_path->pathkeys, path->pathkeys,
						   &better_key) &&
			pathorder_match(new_path->pathorder, path->pathorder,
							&better_sort))
		{

			/*
			 * Replace pathkeys that match exactly, {{1,2}}, {{1,2}}
			 * Replace pathkeys {{1,2}} with {{1,2,3}}} if the latter is
			 * not more expensive and replace unordered path with ordered
			 * path if it is not more expensive.  Favor sorted keys over
			 * unsorted keys in the same way.
			 */
			/* same keys, and new is cheaper, use it */
			if ((better_key == 0 && better_sort == 0 &&
				 new_path->path_cost < path->path_cost) ||

			/* new is better, and cheaper, use it */
				(((better_key == 1 && better_sort != 2) ||
				  (better_key != 2 && better_sort == 1)) &&
				 new_path->path_cost <= path->path_cost))
			{
#ifdef OPTDUP_DEBUG
				printf("replace with new %p old %p better key %d better sort %d\n", &new_path, &path, better_key, better_sort);
				printf("new\n");
				pprint(new_path);
				printf("old\n");
				pprint(path);
#endif
				*is_new = false;
				return path;
			}

			/* same keys, new is more expensive, stop */
			if ((better_key == 0 && better_sort == 0 &&
				 new_path->path_cost >= path->path_cost) ||

			/* old is better, and less expensive, stop */
				(((better_key == 2 && better_sort != 1) ||
				  (better_key != 1 && better_sort == 2)) &&
				 new_path->path_cost >= path->path_cost))
			{
#ifdef OPTDUP_DEBUG
				printf("skip new %p old %p better key %d better sort %d\n", &new_path, &path, better_key, better_sort);
				printf("new\n");
				pprint(new_path);
				printf("old\n");
				pprint(path);
#endif
				*is_new = false;
				return NULL;
			}
		}
	}

#ifdef OPTDUP_DEBUG
	printf("add new %p old %p better key %d better sort %d\n", &new_path, &path, better_key, better_sort);
	printf("new\n");
	pprint(new_path);
#endif

	*is_new = true;
	return NULL;
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
	int			relid = 0;

	Path	   *pathnode = makeNode(Path);

	pathnode->pathtype = T_SeqScan;
	pathnode->parent = rel;
	pathnode->path_cost = 0.0;
	pathnode->pathorder = makeNode(PathOrder);
	pathnode->pathorder->ordtype = SORTOP_ORDER;
	pathnode->pathorder->ord.sortop = NULL;
	pathnode->pathkeys = NIL;

	if (rel->relids != NULL)
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
	pathnode->path.pathorder = makeNode(PathOrder);
	pathnode->path.pathorder->ordtype = SORTOP_ORDER;
	pathnode->path.pathorder->ord.sortop = index->ordering;
	pathnode->path.pathkeys = NIL;

	/* Note that we are making a pathnode for a single-scan indexscan;
	 * therefore, both indexid and indexqual should be single-element
	 * lists.  We initialize indexqual to contain one empty sublist,
	 * representing a single index traversal with no index restriction
	 * conditions.  If we do have restriction conditions to use, they
	 * will get inserted below.
	 */
	Assert(length(index->relids) == 1);
	pathnode->indexid = index->relids;
	pathnode->indexqual = lcons(NIL, NIL);

	pathnode->indexkeys = index->indexkeys;

	/*
	 * The index must have an ordering for the path to have (ordering)
	 * keys, and vice versa.
	 */
	if (pathnode->path.pathorder->ord.sortop)
	{
		pathnode->path.pathkeys = collect_index_pathkeys(index->indexkeys,
														 rel->targetlist);

		/*
		 * Check that the keys haven't 'disappeared', since they may no
		 * longer be in the target list (i.e., index keys that are not
		 * relevant to the scan are not applied to the scan path node, so
		 * if no index keys were found, we can't order the path).
		 */
		if (pathnode->path.pathkeys == NULL)
			pathnode->path.pathorder->ord.sortop = NULL;
	}
	else
		pathnode->path.pathkeys = NULL;

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
		 * restriction clause(s).
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
 * 'outer_path' is the outer join path.
 * 'inner_path' is the inner join path.
 * 'pathkeys' are the keys of the path
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
	pathnode->path.joinid = NIL;
	pathnode->path.outerjoincost = (Cost) 0.0;
	pathnode->path.pathorder = makeNode(PathOrder);

	if (pathkeys)
	{
		pathnode->path.pathorder->ordtype = outer_path->pathorder->ordtype;
		if (outer_path->pathorder->ordtype == SORTOP_ORDER)
			pathnode->path.pathorder->ord.sortop = outer_path->pathorder->ord.sortop;
		else
			pathnode->path.pathorder->ord.merge = outer_path->pathorder->ord.merge;
	}
	else
	{
		pathnode->path.pathorder->ordtype = SORTOP_ORDER;
		pathnode->path.pathorder->ord.sortop = NULL;
	}

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
 * 'pathkeys' are the new keys of the join relation
 * 'order' is the sort order required for the merge
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
					  MergeOrder *order,
					  List *mergeclauses,
					  List *outersortkeys,
					  List *innersortkeys)
{
	MergePath  *pathnode = makeNode(MergePath);

	pathnode->jpath.path.pathtype = T_MergeJoin;
	pathnode->jpath.path.parent = joinrel;
	pathnode->jpath.outerjoinpath = outer_path;
	pathnode->jpath.innerjoinpath = inner_path;
	pathnode->jpath.pathinfo = joinrel->restrictinfo;
	pathnode->jpath.path.pathkeys = pathkeys;
	pathnode->jpath.path.pathorder = makeNode(PathOrder);
	pathnode->jpath.path.pathorder->ordtype = MERGE_ORDER;
	pathnode->jpath.path.pathorder->ord.merge = order;
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
 * 'pathkeys' are the path keys of the new join path
 * 'operator' is the hashjoin operator
 * 'hashclauses' is a list of the hash join clause (always a 1-element list)
 * 'outerkeys' are the sort varkeys for the outer relation
 * 'innerkeys' are the sort varkeys for the inner relation
 * 'innerdisbursion' is an estimate of the disbursion of the inner hash key
 *
 */
HashPath   *
create_hashjoin_path(RelOptInfo *joinrel,
					 int outersize,
					 int innersize,
					 int outerwidth,
					 int innerwidth,
					 Path *outer_path,
					 Path *inner_path,
					 List *pathkeys,
					 Oid operator,
					 List *hashclauses,
					 List *outerkeys,
					 List *innerkeys,
					 Cost innerdisbursion)
{
	HashPath   *pathnode = makeNode(HashPath);

	pathnode->jpath.path.pathtype = T_HashJoin;
	pathnode->jpath.path.parent = joinrel;
	pathnode->jpath.outerjoinpath = outer_path;
	pathnode->jpath.innerjoinpath = inner_path;
	pathnode->jpath.pathinfo = joinrel->restrictinfo;
	pathnode->jpath.path.pathkeys = pathkeys;
	pathnode->jpath.path.pathorder = makeNode(PathOrder);
	pathnode->jpath.path.pathorder->ordtype = SORTOP_ORDER;
	pathnode->jpath.path.pathorder->ord.sortop = NULL;
	pathnode->jpath.path.outerjoincost = (Cost) 0.0;
	pathnode->jpath.path.joinid = (Relids) NULL;
	/* pathnode->hashjoinoperator = operator;  */
	pathnode->path_hashclauses = hashclauses;
	pathnode->outerhashkeys = outerkeys;
	pathnode->innerhashkeys = innerkeys;
	pathnode->jpath.path.path_cost = cost_hashjoin(outer_path->path_cost,
												   inner_path->path_cost,
												   outersize, innersize,
												   outerwidth, innerwidth,
												   innerdisbursion);

	return pathnode;
}
