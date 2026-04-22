/*-------------------------------------------------------------------------
 *
 * repack.h
 *	  header file for the REPACK command
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * src/include/commands/repack.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REPACK_H
#define REPACK_H

#include <signal.h>

#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "storage/lockdefs.h"
#include "utils/relcache.h"


/* flag bits for ClusterParams->options */
#define CLUOPT_VERBOSE 0x01		/* print progress info */
#define CLUOPT_RECHECK 0x02		/* recheck relation state */
#define CLUOPT_RECHECK_ISCLUSTERED 0x04 /* recheck relation state for
										 * indisclustered */
#define CLUOPT_ANALYZE 0x08		/* do an ANALYZE */
#define CLUOPT_CONCURRENT 0x10	/* allow concurrent data changes */

/* options for CLUSTER */
typedef struct ClusterParams
{
	uint32		options;		/* bitmask of CLUOPT_* */
} ClusterParams;

extern PGDLLIMPORT volatile sig_atomic_t RepackMessagePending;


extern void ExecRepack(ParseState *pstate, RepackStmt *stmt, bool isTopLevel);

extern void cluster_rel(RepackCommand cmd, Relation OldHeap, Oid indexOid,
						ClusterParams *params, bool isTopLevel);
extern void check_index_is_clusterable(Relation OldHeap, Oid indexOid,
									   LOCKMODE lockmode);
extern void mark_index_clustered(Relation rel, Oid indexOid, bool is_internal);

extern Oid	make_new_heap(Oid OIDOldHeap, Oid NewTableSpace, Oid NewAccessMethod,
						  char relpersistence, LOCKMODE lockmode);
extern void finish_heap_swap(Oid OIDOldHeap, Oid OIDNewHeap,
							 bool is_system_catalog,
							 bool swap_toast_by_content,
							 bool check_constraints,
							 bool is_internal,
							 bool reindex,
							 TransactionId frozenXid,
							 MultiXactId cutoffMulti,
							 char newrelpersistence);

extern void HandleRepackMessageInterrupt(void);
extern void ProcessRepackMessages(void);

/* in repack_worker.c */
extern void RepackWorkerMain(Datum main_arg);
extern bool AmRepackWorker(void);

#endif							/* REPACK_H */
