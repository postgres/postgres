/*-------------------------------------------------------------------------
 *
 * hashjoin.h
 *	  internal structures for hash joins
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: hashjoin.h,v 1.30 2003/08/04 02:40:13 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HASHJOIN_H
#define HASHJOIN_H

#include "access/htup.h"
#include "storage/buffile.h"

/* ----------------------------------------------------------------
 *				hash-join hash table structures
 *
 * Each active hashjoin has a HashJoinTable control block which is
 * palloc'd in the executor's per-query context.  All other storage needed
 * for the hashjoin is kept in private memory contexts, two for each hashjoin.
 * This makes it easy and fast to release the storage when we don't need it
 * anymore.
 *
 * The hashtable contexts are made children of the per-query context, ensuring
 * that they will be discarded at end of statement even if the join is
 * aborted early by an error.  (Likewise, any temporary files we make will
 * be cleaned up by the virtual file manager in event of an error.)
 *
 * Storage that should live through the entire join is allocated from the
 * "hashCxt", while storage that is only wanted for the current batch is
 * allocated in the "batchCxt".  By resetting the batchCxt at the end of
 * each batch, we free all the per-batch storage reliably and without tedium.
 * ----------------------------------------------------------------
 */

typedef struct HashJoinTupleData
{
	struct HashJoinTupleData *next;		/* link to next tuple in same
										 * bucket */
	HeapTupleData htup;			/* tuple header */
} HashJoinTupleData;

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
	 * Info about the datatype-specific hash functions for the datatypes
	 * being hashed.  We assume that the inner and outer sides of each
	 * hashclause are the same type, or at least share the same hash
	 * function. This is an array of the same length as the number of hash
	 * keys.
	 */
	FmgrInfo   *hashfunctions;	/* lookup data for hash functions */

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

	MemoryContext hashCxt;		/* context for whole-hash-join storage */
	MemoryContext batchCxt;		/* context for this-batch-only storage */
} HashTableData;

typedef HashTableData *HashJoinTable;

#endif   /* HASHJOIN_H */
