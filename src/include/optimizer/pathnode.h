/*-------------------------------------------------------------------------
 *
 * pathnode.h
 *	  prototypes for pathnode.c, relnode.c.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pathnode.h,v 1.52 2003/08/04 02:40:14 momjian Exp $
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

extern Path *create_seqscan_path(Query *root, RelOptInfo *rel);
extern IndexPath *create_index_path(Query *root, RelOptInfo *rel,
				  IndexOptInfo *index,
				  List *restriction_clauses,
				  List *pathkeys,
				  ScanDirection indexscandir);
extern TidPath *create_tidscan_path(Query *root, RelOptInfo *rel,
					List *tideval);
extern AppendPath *create_append_path(RelOptInfo *rel, List *subpaths);
extern ResultPath *create_result_path(RelOptInfo *rel, Path *subpath,
				   List *constantqual);
extern MaterialPath *create_material_path(RelOptInfo *rel, Path *subpath);
extern UniquePath *create_unique_path(Query *root, RelOptInfo *rel,
				   Path *subpath);
extern Path *create_subqueryscan_path(RelOptInfo *rel, List *pathkeys);
extern Path *create_functionscan_path(Query *root, RelOptInfo *rel);

extern NestPath *create_nestloop_path(Query *root,
					 RelOptInfo *joinrel,
					 JoinType jointype,
					 Path *outer_path,
					 Path *inner_path,
					 List *restrict_clauses,
					 List *pathkeys);

extern MergePath *create_mergejoin_path(Query *root,
					  RelOptInfo *joinrel,
					  JoinType jointype,
					  Path *outer_path,
					  Path *inner_path,
					  List *restrict_clauses,
					  List *pathkeys,
					  List *mergeclauses,
					  List *outersortkeys,
					  List *innersortkeys);

extern HashPath *create_hashjoin_path(Query *root,
					 RelOptInfo *joinrel,
					 JoinType jointype,
					 Path *outer_path,
					 Path *inner_path,
					 List *restrict_clauses,
					 List *hashclauses);

/*
 * prototypes for relnode.c
 */
extern void build_base_rel(Query *root, int relid);
extern RelOptInfo *build_other_rel(Query *root, int relid);
extern RelOptInfo *find_base_rel(Query *root, int relid);
extern RelOptInfo *build_join_rel(Query *root,
			   Relids joinrelids,
			   RelOptInfo *outer_rel,
			   RelOptInfo *inner_rel,
			   JoinType jointype,
			   List **restrictlist_ptr);

#endif   /* PATHNODE_H */
