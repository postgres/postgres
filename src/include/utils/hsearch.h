/*-------------------------------------------------------------------------
 *
 * hsearch.h--
 *	  for hashing in the new buffer manager
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: hsearch.h,v 1.9.2.1 1999/03/07 02:01:09 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HSEARCH_H
#define HSEARCH_H


/*
 * Constants
 *
 * A hash table has a top-level "directory", each of whose entries points
 * to a "segment" of ssize bucket headers.  The maximum number of hash
 * buckets is thus dsize * ssize (but dsize may be expansible).  Of course,
 * the number of records in the table can be larger, but we don't want a
 * whole lot of records per bucket or performance goes down.
 *
 * In a hash table allocated in shared memory, the directory cannot be
 * expanded because it must stay at a fixed address.
 */
#define DEF_SEGSIZE			   256
#define DEF_SEGSIZE_SHIFT	   8			/* log2(SEGSIZE)  */
#define DEF_DIRSIZE			   256
#define DEF_FFACTOR			   1			/* default fill factor */

#define PRIME1				   37			/* for the hash function */
#define PRIME2				   1048583


/*
 * Hash bucket is actually bigger than this.  Key field can have
 * variable length and a variable length data field follows it.
 */
typedef struct element
{
	unsigned long next;			/* secret from user		 */
	long		key;
} ELEMENT;

typedef unsigned long BUCKET_INDEX;

/* segment is an array of bucket pointers  */
typedef BUCKET_INDEX *SEGMENT;
typedef unsigned long SEG_OFFSET;

typedef struct hashhdr
{
	long		dsize;			/* Directory Size */
	long		ssize;			/* Segment Size --- must be power of 2 */
	long		sshift;			/* Segment shift */
	long		max_bucket;		/* ID of Maximum bucket in use */
	long		high_mask;		/* Mask to modulo into entire table */
	long		low_mask;		/* Mask to modulo into lower half of table */
	long		ffactor;		/* Fill factor */
	long		nkeys;			/* Number of keys in hash table */
	long		nsegs;			/* Number of allocated segments */
	long		keysize;		/* hash key length in bytes */
	long		datasize;		/* elem data length in bytes */
	long		max_dsize;		/* 'dsize' limit if directory is fixed size */
	BUCKET_INDEX freeBucketIndex;
	/* index of first free bucket */
#ifdef HASH_STATISTICS
	long		accesses;
	long		collisions;
#endif
} HHDR;

typedef struct htab
{
	HHDR	   *hctl;			/* shared control information */
	long		(*hash) ();		/* Hash Function */
	char	   *segbase;		/* segment base address for calculating
								 * pointer values */
	SEG_OFFSET *dir;			/* 'directory' of segm starts */
	long	   *(*alloc) ();	/* memory allocator (long * for alignment
								 * reasons) */

} HTAB;

typedef struct hashctl
{
	long		ssize;			/* Segment Size */
	long		dsize;			/* Dirsize Size */
	long		ffactor;		/* Fill factor */
	long		(*hash) ();		/* Hash Function */
	long		keysize;		/* hash key length in bytes */
	long		datasize;		/* elem data length in bytes */
	long		max_dsize;		/* limit to dsize if directory size is
								 * limited */
	long	   *segbase;		/* base for calculating bucket + seg ptrs */
	long	   *(*alloc) ();	/* memory allocation function */
	long	   *dir;			/* directory if allocated already */
	long	   *hctl;			/* location of header information in shd
								 * mem */
} HASHCTL;

/* Flags to indicate action for hctl */
#define HASH_SEGMENT	0x002	/* Setting segment size */
#define HASH_DIRSIZE	0x004	/* Setting directory size */
#define HASH_FFACTOR	0x008	/* Setting fill factor */
#define HASH_FUNCTION	0x010	/* Set user defined hash function */
#define HASH_ELEM		0x020	/* Setting key/data size */
#define HASH_SHARED_MEM 0x040	/* Setting shared mem const */
#define HASH_ATTACH		0x080	/* Do not initialize hctl */
#define HASH_ALLOC		0x100	/* Setting memory allocator */


/* seg_alloc assumes that INVALID_INDEX is 0*/
#define INVALID_INDEX			(0)
#define NO_MAX_DSIZE			(-1)
/* number of hash buckets allocated at once */
#define BUCKET_ALLOC_INCR		(30)

/* hash_search operations */
typedef enum
{
	HASH_FIND,
	HASH_ENTER,
	HASH_REMOVE,
	HASH_FIND_SAVE,
	HASH_REMOVE_SAVED
} HASHACTION;

/*
 * prototypes from functions in dynahash.c
 */
extern HTAB *hash_create(int nelem, HASHCTL *info, int flags);
extern void hash_destroy(HTAB *hashp);
extern void hash_stats(char *where, HTAB *hashp);
extern long *hash_search(HTAB *hashp, char *keyPtr, HASHACTION action,
			bool *foundPtr);
extern long *hash_seq(HTAB *hashp);
extern long hash_estimate_size(long num_entries, long keysize, long datasize);

/*
 * prototypes from functions in hashfn.c
 */
extern long string_hash(char *key, int keysize);
extern long tag_hash(int *key, int keysize);

#endif	 /* HSEARCH_H */
