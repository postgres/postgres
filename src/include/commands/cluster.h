/*-------------------------------------------------------------------------
 *
 * cluster.h
 *	  header file for postgres cluster command stuff
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/cluster.h,v 1.22 2004/05/06 16:10:57 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLUSTER_H
#define CLUSTER_H

#include "nodes/parsenodes.h"
#include "utils/rel.h"


extern void cluster(ClusterStmt *stmt);

extern void check_index_is_clusterable(Relation OldHeap, Oid indexOid);
extern void rebuild_relation(Relation OldHeap, Oid indexOid);
extern Oid	make_new_heap(Oid OIDOldHeap, const char *NewName);
extern List *get_indexattr_list(Relation OldHeap, Oid *OldClusterIndex);
extern void rebuild_indexes(Oid OIDOldHeap, List *indexes,
							Oid OIDClusterIndex);
extern void swap_relfilenodes(Oid r1, Oid r2);

#endif   /* CLUSTER_H */
