/*-------------------------------------------------------------------------
 *
 * hashjoin.h
 *	  internal structures for hash joins
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: hashjoin.h,v 1.12 1999/05/25 16:13:54 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HASHJOIN_H
#define HASHJOIN_H

#include "access/htup.h"
#include "storage/fd.h"
#include "utils/mcxt.h"

/* ----------------------------------------------------------------
 *				hash-join hash table structures
 *
 * Each active hashjoin has a HashJoinTable control block which is
 * palloc'd in the executor's context.	All other storage needed for
 * the hashjoin is kept in a private "named portal", one for each hashjoin.
 * This makes it easy and fast to release the storage when we don't need it
 * anymore.
 *
 * The portal manager guarantees that portals will be discarded at end of
 * transaction, so we have no problem with a memory leak if the join is
 * aborted early by an error.  (Likewise, any temporary files we make will
 * be cleaned up by the virtual file manager in event of an error.)
 *
 * Storage that should live through the entire join is allocated from the
 * portal's "variable context", while storage that is only wanted for the
 * current batch is allocated in the portal's "heap context".  By popping
 * the portal's heap at the end of a batch, we free all the per-batch storage
 * reliably and without tedium.
 * ----------------------------------------------------------------
 */

typedef struct HashJoinTupleData
{
	struct HashJoinTupleData *next;		/* link to next tuple in same
										 * bucket */
	HeapTupleData htup;			/* tuple header */
}			HashJoinTupleData;

typedef HashJoinTupleData *HashJoinTuple;

typedef struct HashTableData
{
	int			nbuckets;		/* buckets in use during this batch */
	int			totalbuckets;	/* total number of (virtual) buckets */
	HashJoinTuple *buckets;		/* buckets[i] is head of list of tuples */
	/* buckets array is per-batch storage, as are all the tuples */

	int			nbatch;			/* number of batches; 0 means 1-pass join */
	int			curbatch;		/* current batch #, or 0 during 1st pass */

	/*
	 * all these arrays are allocated for the life of the hash join, but
	 * only if nbatch > 0:
	 */
	BufFile   **innerBatchFile; /* buffered virtual temp file per batch */
	BufFile   **outerBatchFile; /* buffered virtual temp file per batch */
	long	   *outerBatchSize; /* count of tuples in each outer batch
								 * file */
	long	   *innerBatchSize; /* count of tuples in each inner batch
								 * file */

	/*
	 * During 1st scan of inner relation, we get tuples from executor. If
	 * nbatch > 0 then tuples that don't belong in first nbuckets logical
	 * buckets get dumped into inner-batch temp files. The same statements
	 * apply for the 1st scan of the outer relation, except we write
	 * tuples to outer-batch temp files. If nbatch > 0 then we do the
	 * following for each batch: 1. Read tuples from inner batch file,
	 * load into hash buckets. 2. Read tuples from outer batch file, match
	 * to hash buckets and output.
	 */

	/*
	 * Ugly kluge: myPortal ought to be declared as type Portal (ie,
	 * PortalD*) but if we try to include utils/portal.h here, we end up
	 * with a circular dependency of include files!  Until the various
	 * node.h files are restructured in a cleaner way, we have to fake it.
	 * The most reliable fake seems to be to declare myPortal as void *
	 * and then cast it to the right things in nodeHash.c.
	 */
	void	   *myPortal;		/* where to keep working storage */
	MemoryContext hashCxt;		/* context for whole-hash-join storage */
	MemoryContext batchCxt;		/* context for this-batch-only storage */
} HashTableData;

typedef HashTableData *HashJoinTable;

#endif	 /* HASHJOIN_H */
