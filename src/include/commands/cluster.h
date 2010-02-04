/*-------------------------------------------------------------------------
 *
 * cluster.h
 *	  header file for postgres cluster command stuff
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/cluster.h,v 1.39 2010/02/04 00:09:14 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLUSTER_H
#define CLUSTER_H

#include "nodes/parsenodes.h"
#include "utils/relcache.h"


extern void cluster(ClusterStmt *stmt, bool isTopLevel);
extern void cluster_rel(Oid tableOid, Oid indexOid, bool recheck,
					bool verbose, int freeze_min_age, int freeze_table_age);
extern void check_index_is_clusterable(Relation OldHeap, Oid indexOid,
						   bool recheck);
extern void mark_index_clustered(Relation rel, Oid indexOid);

extern Oid	make_new_heap(Oid OIDOldHeap, Oid NewTableSpace);
extern void swap_relation_files(Oid r1, Oid r2, bool swap_toast_by_content,
					TransactionId frozenXid);
extern void cleanup_heap_swap(Oid OIDOldHeap, Oid OIDNewHeap,
							  bool swap_toast_by_content);

#endif   /* CLUSTER_H */
