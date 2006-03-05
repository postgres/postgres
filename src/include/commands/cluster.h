/*-------------------------------------------------------------------------
 *
 * cluster.h
 *	  header file for postgres cluster command stuff
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/cluster.h,v 1.30 2006/03/05 15:58:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLUSTER_H
#define CLUSTER_H

#include "nodes/parsenodes.h"
#include "utils/rel.h"


extern void cluster(ClusterStmt *stmt);

extern void check_index_is_clusterable(Relation OldHeap, Oid indexOid,
						   bool recheck);
extern void mark_index_clustered(Relation rel, Oid indexOid);
extern Oid make_new_heap(Oid OIDOldHeap, const char *NewName,
			  Oid NewTableSpace);
extern void swap_relation_files(Oid r1, Oid r2);

#endif   /* CLUSTER_H */
