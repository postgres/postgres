/*-------------------------------------------------------------------------
 *
 * paths.h--
 *	  prototypes for various files in optimizer/paths (were separate
 *	  header files
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: paths.h,v 1.9 1998/07/18 04:22:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PATHS_H
#define PATHS_H

#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/relation.h"

/*
 * allpaths.h
 */
extern List *find_paths(Query *root, List *rels);

/*
 * indxpath.h
 *	  routines to generate index paths
 */
extern List *
find_index_paths(Query *root, RelOptInfo *rel, List *indices,
				 List *clauseinfo_list,
				 List *joininfo_list);

/*
 * joinpath.h
 *	   routines to create join paths
 */
extern void find_all_join_paths(Query *root, List *joinrels);


/*
 * orindxpath.h
 */
extern List *create_or_index_paths(Query *root, RelOptInfo *rel, List *clauses);

/*
 * hashutils.h
 *	  routines to deal with hash keys and clauses
 */
extern List *
group_clauses_by_hashop(List *clauseinfo_list,
						int inner_relid);

/*
 * joinutils.h
 *	  generic join method key/clause routines
 */
extern List *
match_pathkeys_joinkeys(List *pathkeys,
					 List *joinkeys, List *joinclauses, int which_subkey,
						List **matchedJoinClausesPtr);
extern List *
extract_path_keys(List *joinkeys, List *tlist,
				  int which_subkey);
extern Path *
match_paths_joinkeys(List *joinkeys, PathOrder *ordering,
					 List *paths, int which_subkey);
extern List *
new_join_pathkeys(List *outer_pathkeys,
				  List *join_rel_tlist, List *joinclauses);

/*
 * mergeutils.h
 *	  routines to deal with merge keys and clauses
 */
extern List *
group_clauses_by_order(List *clauseinfo_list,
					   int inner_relid);
extern MInfo *
match_order_mergeinfo(PathOrder *ordering,
					  List *mergeinfo_list);

/*
 * joinrels.h
 *	  routines to determine which relations to join
 */
extern List *find_join_rels(Query *root, List *outer_rels);
extern void add_new_joininfos(Query *root, List *joinrels, List *outerrels);
extern List *final_join_rels(List *join_rel_list);

/*
 * prototypes for path/prune.c
 */
extern void prune_joinrels(List *rel_list);
extern void prune_rel_paths(List *rel_list);
extern Path *prune_rel_path(RelOptInfo *rel, Path *unorderedpath);
extern List *merge_joinrels(List *rel_list1, List *rel_list2);
extern List *prune_oldrels(List *old_rels);

#endif							/* PATHS_H */
