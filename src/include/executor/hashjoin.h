/*-------------------------------------------------------------------------
 *
 * hashjoin.h--
 *	  internal structures for hash table and buckets
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: hashjoin.h,v 1.7 1998/09/01 04:35:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HASHJOIN_H
#define HASHJOIN_H

#include <storage/ipc.h>

/* -----------------
 *	have to use relative address as pointers in the hashtable
 *	because the hashtable may reallocate in difference processes
 * -----------------
 */
typedef int RelativeAddr;

/* ------------------
 *	the relative addresses are always relative to the head of the
 *	hashtable, the following macro converts them to absolute address.
 * ------------------
 */
#define ABSADDR(X)		((X) < 0 ? NULL: (char*)hashtable + X)
#define RELADDR(X)		(RelativeAddr)((char*)(X) - (char*)hashtable)

typedef char **charPP;
typedef int *intP;

/* ----------------------------------------------------------------
 *				hash-join hash table structures
 * ----------------------------------------------------------------
 */
typedef struct HashTableData
{
	int			nbuckets;
	int			totalbuckets;
	int			bucketsize;
	IpcMemoryId shmid;
	RelativeAddr top;			/* char* */
	RelativeAddr bottom;		/* char* */
	RelativeAddr overflownext;	/* char* */
	RelativeAddr batch;			/* char* */
	RelativeAddr readbuf;		/* char* */
	int			nbatch;
	RelativeAddr outerbatchNames;		/* RelativeAddr* */
	RelativeAddr outerbatchPos; /* RelativeAddr* */
	RelativeAddr innerbatchNames;		/* RelativeAddr* */
	RelativeAddr innerbatchPos; /* RelativeAddr* */
	RelativeAddr innerbatchSizes;		/* int* */
	int			curbatch;
	int			nprocess;
	int			pcount;
} HashTableData;				/* real hash table follows here */

typedef HashTableData *HashJoinTable;

typedef struct OverflowTupleData
{
	RelativeAddr tuple;			/* HeapTuple */
	RelativeAddr next;			/* struct OverflowTupleData * */
} OverflowTupleData;			/* real tuple follows here */

typedef OverflowTupleData *OverflowTuple;

typedef struct HashBucketData
{
	RelativeAddr top;			/* HeapTuple */
	RelativeAddr bottom;		/* HeapTuple */
	RelativeAddr firstotuple;	/* OverflowTuple */
	RelativeAddr lastotuple;	/* OverflowTuple */
} HashBucketData;				/* real bucket follows here */

typedef HashBucketData *HashBucket;

#define HASH_PERMISSION			0700

#endif	 /* HASHJOIN_H */
