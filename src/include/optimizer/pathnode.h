/*-------------------------------------------------------------------------
 *
 * pathnode.h
 *	  prototypes for pathnode.c, indexnode.c, relnode.c.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pathnode.h,v 1.26 2000/02/15 20:49:26 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PATHNODE_H
#define PATHNODE_H

#include "nodes/relation.h"

/*
 * prototypes for pathnode.c
 */
extern int compare_path_costs(Path *path1, Path *path2,
							  CostSelector criterion);
extern int compare_fractional_path_costs(Path *path1, Path *path2,
										 double fraction);
extern void set_cheapest(RelOptInfo *parent_rel);
extern void add_path(RelOptInfo *parent_rel, Path *new_path);

extern Path *create_seqscan_path(RelOptInfo *rel);
extern IndexPath *create_index_path(Query *root, RelOptInfo *rel,
									IndexOptInfo *index,
									List *restriction_clauses,
									ScanDirection indexscandir);
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
