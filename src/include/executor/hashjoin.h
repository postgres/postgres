/*-------------------------------------------------------------------------
 *
 * hashjoin.h
 *	  internal structures for hash table and buckets
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: hashjoin.h,v 1.9 1999/05/06 00:30:45 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HASHJOIN_H
#define HASHJOIN_H

#include <storage/ipc.h>

/* -----------------
 *	have to use relative address as pointers in the hashtable
 *	because the hashtable may reallocate in different processes
 *
 *  XXX: this relative-address stuff is useless on all supported platforms
 *  and is a ever-dangerous source of bugs.  Really ought to rip it out.
 * -----------------
 */
typedef int RelativeAddr;

/* ------------------
 *	The relative addresses are always relative to the head of the
 *	hashtable, the following macros convert them to/from absolute address.
 *  NULL is represented as -1 (CAUTION: RELADDR() doesn't handle that!).
 *  CAUTION: ABSADDR evaluates its arg twice!!
 * ------------------
 */
#define ABSADDR(X)		((X) < 0 ? (char*) NULL : (char*)hashtable + (X))
#define RELADDR(X)		((RelativeAddr)((char*)(X) - (char*)hashtable))

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

#endif	 /* HASHJOIN_H */
