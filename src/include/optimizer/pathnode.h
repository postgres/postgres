/*-------------------------------------------------------------------------
 *
 * pathnode.h--
 *	  prototypes for pathnode.c, indexnode.c, relnode.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pathnode.h,v 1.5 1997/11/26 01:13:46 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PATHNODE_H
#define PATHNODE_H

#include "nodes/nodes.h"
#include "nodes/relation.h"
#include "nodes/parsenodes.h"

/*
 * prototypes for pathnode.c
 */
extern bool path_is_cheaper(Path *path1, Path *path2);
extern Path *set_cheapest(Rel *parent_rel, List *pathlist);
extern List *
add_pathlist(Rel *parent_rel, List *unique_paths,
			 List *new_paths);
extern Path *create_seqscan_path(Rel *rel);
extern IndexPath *
create_index_path(Query *root, Rel *rel, Rel *index,
				  List *restriction_clauses, bool is_join_scan);
extern JoinPath *
create_nestloop_path(Rel *joinrel, Rel *outer_rel,
					 Path *outer_path, Path *inner_path, List *keys);
extern MergePath *
create_mergesort_path(Rel *joinrel, int outersize,
		 int innersize, int outerwidth, int innerwidth, Path *outer_path,
					  Path *inner_path, List *keys, MergeOrder *order,
		   List *mergeclauses, List *outersortkeys, List *innersortkeys);

extern HashPath *
create_hashjoin_path(Rel *joinrel, int outersize,
		 int innersize, int outerwidth, int innerwidth, Path *outer_path,
		   Path *inner_path, List *keys, Oid operator, List *hashclauses,
					 List *outerkeys, List *innerkeys);

/*
 * prototypes for rel.c
 */
extern Rel *rel_member(List *relid, List *rels);
extern Rel *get_base_rel(Query *root, int relid);
extern Rel *get_join_rel(Query *root, List *relid);

/*
 * prototypes for indexnode.h
 */
extern List *find_relation_indices(Query *root, Rel *rel);

#endif							/* PATHNODE_H */
