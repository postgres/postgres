/*-------------------------------------------------------------------------
 *
 * pathnode.c--
 *	  Routines to manipulate pathlists and create path nodes
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/pathnode.c,v 1.32 1999/02/12 02:37:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>

#include "postgres.h"

#include "nodes/relation.h"
#include "utils/elog.h"

#include "optimizer/internal.h"
#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/plancat.h"
#include "optimizer/cost.h"
#include "optimizer/keys.h"
#include "optimizer/xfunc.h"
#include "optimizer/ordering.h"

#include "parser/parsetree.h"	/* for getrelid() */

static Path *better_path(Path *new_path, List *unique_paths, bool *is_new);


/*****************************************************************************
 *		MISC. PATH UTILITIES
 *****************************************************************************/

/*
 * path-is-cheaper--
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
 * set_cheapest--
 *	  Finds the minimum cost path from among a relation's paths.
 *
 * 'parent-rel' is the parent relation
 * 'pathlist' is a list of path nodes corresponding to 'parent-rel'
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
 * add_pathlist--
 *	  For each path in the list 'new-paths', add to the list 'unique-paths'
 *	  only those paths that are unique (i.e., unique ordering and ordering
 *	  keys).  Should a conflict arise, the more expensive path is thrown out,
 *	  thereby pruning the plan space.  But we don't prune if xfunc
 *	  told us not to.
 *
 * 'parent-rel' is the relation entry to which these paths correspond.
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
 * better_path--
 *	  Determines whether 'new-path' has the same ordering and keys as some
 *	  path in the list 'unique-paths'.	If there is a redundant path,
 *	  eliminate the more expensive path.
 *
 * Returns:
 *	  The old path - if 'new-path' matches some path in 'unique-paths' and is
 *				cheaper
 *	  nil - if 'new-path' matches but isn't cheaper
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
	
	foreach(temp, unique_paths)
	{
		path = (Path *) lfirst(temp);

#ifdef OPTDUP_DEBUG
		if (!pathkeys_match(new_path->pathkeys, path->pathkeys, &better_key) ||
		    better_key != 0)
		{
			printf("oldpath\n");
			pprint(path->pathkeys);
			printf("newpath\n");
			pprint(new_path->pathkeys);
			if (path->pathkeys && new_path->pathkeys &&
				length(lfirst(path->pathkeys)) >= 2/* &&
				length(lfirst(path->pathkeys)) <
				length(lfirst(new_path->pathkeys))*/)
				sleep(0); /* set breakpoint here */
		}
		if (!pathorder_match(new_path->pathorder, path->pathorder,
			&better_sort) ||
			better_sort != 0)
		{
			printf("oldord\n");
			pprint(path->pathorder);
			printf("neword\n");
			pprint(new_path->pathorder);
		}
#endif

		if (pathkeys_match(new_path->pathkeys, path->pathkeys,
			&better_key) &&
			pathorder_match(new_path->pathorder, path->pathorder,
			&better_sort))
		{
			/*
			 * Replace pathkeys that match exactly, (1,2), (1,2).
			 * Replace pathkeys (1,2) with (1,2,3) if the latter is not
			 * more expensive and replace unordered path with ordered
			 * path if it is not more expensive.  Favor sorted keys
			 * over unsorted keys in the same way.
			 */
							/* same keys, and new is cheaper, use it */
		    if ((better_key == 0 && better_sort == 0 &&
				 new_path->path_cost < path->path_cost) ||

							/* new is better, and cheaper, use it */
				(((better_key == 1 && better_sort != 2) ||
				  (better_key != 2 && better_sort == 1)) &&
				  new_path->path_cost <= path->path_cost))
			{
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
#ifdef OPTDB_DEBUG
				printf("better key %d better sort %d\n", better_key, better_sort);
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

	*is_new = true;
	return NULL;
}



/*****************************************************************************
 *		PATH NODE CREATION ROUTINES
 *****************************************************************************/

/*
 * create_seqscan_path--
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

	/*
	 * copy restrictinfo list into path for expensive function processing --
	 * JMH, 7/7/92
	 */
	pathnode->loc_restrictinfo = (List *) copyObject((Node *) rel->restrictinfo);

	if (rel->relids != NULL)
		relid = lfirsti(rel->relids);

	pathnode->path_cost = cost_seqscan(relid,
									   rel->pages, rel->tuples);
	/* add in expensive functions cost!  -- JMH, 7/7/92 */
#if 0
	if (XfuncMode != XFUNC_OFF)
	{
		pathnode->path_cost += xfunc_get_path_cost(pathnode);
	}
#endif
	return pathnode;
}

/*
 * create_index_path--
 *	  Creates a single path node for an index scan.
 *
 * 'rel' is the parent rel
 * 'index' is the pathnode for the index on 'rel'
 * 'restriction-clauses' is a list of restriction clause nodes.
 * 'is-join-scan' is a flag indicating whether or not the index is being
 *		considered because of its sort order.
 *
 * Returns the new path node.
 *
 */
IndexPath  *
create_index_path(Query *root,
				  RelOptInfo *rel,
				  RelOptInfo *index,
				  List *restriction_clauses,
				  bool is_join_scan)
{
	IndexPath  *pathnode = makeNode(IndexPath);

	pathnode->path.pathtype = T_IndexScan;
	pathnode->path.parent = rel;
	pathnode->path.pathorder = makeNode(PathOrder);
	pathnode->path.pathorder->ordtype = SORTOP_ORDER;
	pathnode->path.pathorder->ord.sortop = index->ordering;

	pathnode->indexid = index->relids;
	pathnode->indexkeys = index->indexkeys;
	pathnode->indexqual = NIL;

	/*
	 * copy restrictinfo list into path for expensive function processing --
	 * JMH, 7/7/92
	 */
	pathnode->path.loc_restrictinfo = set_difference((List *) copyObject((Node *) rel->restrictinfo),
					   (List *) restriction_clauses);

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

	if (is_join_scan || restriction_clauses == NULL)
	{

		/*
		 * Indices used for joins or sorting result nodes don't restrict
		 * the result at all, they simply order it, so compute the scan
		 * cost accordingly -- use a selectivity of 1.0.
		 */
/* is the statement above really true?	what about IndexScan as the
   inner of a join? */
		pathnode->path.path_cost = cost_index(lfirsti(index->relids),
					   index->pages,
					   1.0,
					   rel->pages,
					   rel->tuples,
					   index->pages,
					   index->tuples,
					   false);
		/* add in expensive functions cost!  -- JMH, 7/7/92 */
#if 0
		if (XfuncMode != XFUNC_OFF)
		{
			pathnode->path_cost = (pathnode->path_cost +
				 xfunc_get_path_cost((Path *) pathnode));
		}
#endif
	}
	else
	{

		/*
		 * Compute scan cost for the case when 'index' is used with a
		 * restriction clause.
		 */
		List	   *attnos;
		List	   *values;
		List	   *flags;
		float		npages;
		float		selec;
		Cost		clausesel;

		get_relattvals(restriction_clauses,
					   &attnos,
					   &values,
					   &flags);
		index_selectivity(lfirsti(index->relids),
						  index->classlist,
						  get_opnos(restriction_clauses),
						  getrelid(lfirsti(rel->relids),
								   root->rtable),
						  attnos,
						  values,
						  flags,
						  length(restriction_clauses),
						  &npages,
						  &selec);
		/* each clause gets an equal selectivity */
		clausesel =	pow(selec, 1.0 / (double) length(restriction_clauses));

		pathnode->indexqual = restriction_clauses;
		pathnode->path.path_cost = cost_index(lfirsti(index->relids),
											   (int) npages,
											   selec,
											   rel->pages,
											   rel->tuples,
											   index->pages,
											   index->tuples,
											   false);

#if 0
		/* add in expensive functions cost!  -- JMH, 7/7/92 */
		if (XfuncMode != XFUNC_OFF)
		{
			pathnode->path_cost += xfunc_get_path_cost((Path *) pathnode);
		}
#endif

		/*
		 * Set selectivities of clauses used with index to the selectivity
		 * of this index, subdividing the selectivity equally over each of
		 * the clauses.
		 */

		/* XXX Can this divide the selectivities in a better way? */
		set_clause_selectivities(restriction_clauses, clausesel);
	}
	return pathnode;
}

/*
 * create_nestloop_path--
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
JoinPath   *
create_nestloop_path(RelOptInfo *joinrel,
					 RelOptInfo *outer_rel,
					 Path *outer_path,
					 Path *inner_path,
					 List *pathkeys)
{
	JoinPath   *pathnode = makeNode(JoinPath);

	pathnode->path.pathtype = T_NestLoop;
	pathnode->path.parent = joinrel;
	pathnode->outerjoinpath = outer_path;
	pathnode->innerjoinpath = inner_path;
	pathnode->pathinfo = joinrel->restrictinfo;
	pathnode->path.pathkeys = pathkeys;
	pathnode->path.joinid = NIL;
	pathnode->path.outerjoincost = (Cost) 0.0;
	pathnode->path.loc_restrictinfo = NIL;
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
	/* add in expensive function costs -- JMH 7/7/92 */
#if 0
	if (XfuncMode != XFUNC_OFF)
		pathnode->path_cost += xfunc_get_path_cost((Path *) pathnode);
#endif
	return pathnode;
}

/*
 * create_mergejoin_path--
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
	pathnode->jpath.path.loc_restrictinfo = NIL;
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
	/* add in expensive function costs -- JMH 7/7/92 */
#if 0
	if (XfuncMode != XFUNC_OFF)
	{
		pathnode->path_cost += xfunc_get_path_cost((Path *) pathnode);
	}
#endif
	return pathnode;
}

/*
 * create_hashjoin_path--						XXX HASH
 *	  Creates a pathnode corresponding to a hash join between two relations.
 *
 * 'joinrel' is the join relation
 * 'outersize' is the number of tuples in the outer relation
 * 'innersize' is the number of tuples in the inner relation
 * 'outerwidth' is the number of bytes per tuple in the outer relation
 * 'innerwidth' is the number of bytes per tuple in the inner relation
 * 'outer_path' is the outer path
 * 'inner_path' is the inner path
 * 'pathkeys' are the new keys of the join relation
 * 'operator' is the hashjoin operator
 * 'hashclauses' are the applicable join/restriction clauses
 * 'outerkeys' are the sort varkeys for the outer relation
 * 'innerkeys' are the sort varkeys for the inner relation
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
					 List *innerkeys)
{
	HashPath   *pathnode = makeNode(HashPath);

	pathnode->jpath.path.pathtype = T_HashJoin;
	pathnode->jpath.path.parent = joinrel;
	pathnode->jpath.outerjoinpath = outer_path;
	pathnode->jpath.innerjoinpath = inner_path;
	pathnode->jpath.pathinfo = joinrel->restrictinfo;
	pathnode->jpath.path.loc_restrictinfo = NIL;
	pathnode->jpath.path.pathkeys = pathkeys;
	pathnode->jpath.path.pathorder = makeNode(PathOrder);
	pathnode->jpath.path.pathorder->ordtype = SORTOP_ORDER;
	pathnode->jpath.path.pathorder->ord.sortop = NULL;
	pathnode->jpath.path.outerjoincost = (Cost) 0.0;
	pathnode->jpath.path.joinid = (Relid) NULL;
	/* pathnode->hashjoinoperator = operator;  */
	pathnode->path_hashclauses = hashclauses;
	pathnode->outerhashkeys = outerkeys;
	pathnode->innerhashkeys = innerkeys;
	pathnode->jpath.path.path_cost = cost_hashjoin(outer_path->path_cost,
					  inner_path->path_cost,
					  outerkeys,
					  innerkeys,
					  outersize, innersize,
					  outerwidth, innerwidth);
	/* add in expensive function costs -- JMH 7/7/92 */
#if 0
	if (XfuncMode != XFUNC_OFF)
	{
		pathnode->path_cost += xfunc_get_path_cost((Path *) pathnode);
	}
#endif
	return pathnode;
}
