/*-------------------------------------------------------------------------
 *
 * pathnode.h
 *	  prototypes for pathnode.c, indexnode.c, relnode.c.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pathnode.h,v 1.25 2000/02/07 04:41:04 tgl Exp $
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
extern void add_path(RelOptInfo *parent_rel, Path *new_path);
extern void add_pathlist(RelOptInfo *parent_rel, List *new_paths);

extern Path *create_seqscan_path(RelOptInfo *rel);
extern IndexPath *create_index_path(Query *root, RelOptInfo *rel,
									IndexOptInfo *index,
									List *restriction_clauses);
extern TidPath *create_tidscan_path(RelOptInfo *rel, List *tideval);

extern NestPath *create_nestloop_path(RelOptInfo *joinrel,
									  Path *outer_path,
									  Path *inner_path,
									  List *restrict_clauses,
									  List *pathkeys);

extern MergePath *create_mergejoin_path(RelOptInfo *joinrel,
										Path *outer_path,
										Path *inner_path,
										List *restrict_clauses,
										List *pathkeys,
										List *mergeclauses,
										List *outersortkeys,
										List *innersortkeys);

extern HashPath *create_hashjoin_path(RelOptInfo *joinrel,
									  Path *outer_path,
									  Path *inner_path,
									  List *restrict_clauses,
									  List *hashclauses,
									  Selectivity innerdisbursion);

/*
 * prototypes for relnode.c
 */
extern RelOptInfo *get_base_rel(Query *root, int relid);
extern RelOptInfo *get_join_rel(Query *root, RelOptInfo *outer_rel,
								RelOptInfo *inner_rel,
								List **restrictlist_ptr);

/*
 * prototypes for indexnode.h
 */
extern List *find_relation_indices(Query *root, RelOptInfo *rel);

#endif	 /* PATHNODE_H */
