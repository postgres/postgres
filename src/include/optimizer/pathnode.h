/*-------------------------------------------------------------------------
 *
 * pathnode.h
 *	  prototypes for pathnode.c, indexnode.c, relnode.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pathnode.h,v 1.22 1999/11/23 20:07:06 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PATHNODE_H
#define PATHNODE_H

#include "nodes/relation.h"

/*
 * prototypes for pathnode.c
 */
extern bool path_is_cheaper(Path *path1, Path *path2);
extern Path *set_cheapest(RelOptInfo *parent_rel, List *pathlist);
extern List *add_pathlist(RelOptInfo *parent_rel, List *old_paths,
			  List *new_paths);

extern Path *create_seqscan_path(RelOptInfo *rel);

extern IndexPath *create_index_path(Query *root, RelOptInfo *rel,
		RelOptInfo *index, List *restriction_clauses);
extern TidPath *create_tidscan_path(RelOptInfo *rel, List *tideval);

extern NestPath *create_nestloop_path(RelOptInfo *joinrel,
		RelOptInfo *outer_rel, Path *outer_path, Path *inner_path,
		List *pathkeys);

extern MergePath *create_mergejoin_path(RelOptInfo *joinrel, int outersize,
		int innersize, int outerwidth, int innerwidth, Path *outer_path,
		Path *inner_path, List *pathkeys,
		List *mergeclauses, List *outersortkeys, List *innersortkeys);

extern HashPath *create_hashjoin_path(RelOptInfo *joinrel, int outersize,
		int innersize, int outerwidth, int innerwidth, Path *outer_path,
		Path *inner_path, List *hashclauses, Cost innerdisbursion);

/*
 * prototypes for rel.c
 */
extern RelOptInfo *rel_member(Relids relid, List *rels);
extern RelOptInfo *get_base_rel(Query *root, int relid);
extern RelOptInfo *get_join_rel(Query *root, Relids relid);

/*
 * prototypes for indexnode.h
 */
extern List *find_relation_indices(Query *root, RelOptInfo *rel);

#endif	 /* PATHNODE_H */
