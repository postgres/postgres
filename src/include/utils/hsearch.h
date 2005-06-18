/*-------------------------------------------------------------------------
 *
 * hsearch.h
 *	  for hash tables, particularly hash tables in shared memory
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: hsearch.h,v 1.29.2.1 2005/06/18 20:51:59 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HSEARCH_H
#define HSEARCH_H


/*
 * Hash and comparison functions must have these signatures.  Comparison
 * functions return zero for match, nonzero for no match.  (The comparison
 * function definition is designed to allow memcmp() and strncmp() to be
 * used directly as key comparison functions.)
 */
typedef uint32 (*HashValueFunc) (const void *key, Size keysize);
typedef int (*HashCompareFunc) (const void *key1, const void *key2,
								Size keysize);
typedef void *(*HashCopyFunc) (void *dest, const void *src, Size keysize);

/*
 * Space allocation function for a hashtable --- designed to match malloc().
 * Note: there is no free function API; can't destroy a hashtable unless you
 * use the default allocator.
 */
typedef void *(*HashAllocFunc) (Size request);

/*
 * Constants
 *
 * A hash table has a top-level "directory", each of whose entries points
 * to a "segment" of ssize bucket headers.	The maximum number of hash
 * buckets is thus dsize * ssize (but dsize may be expansible).  Of course,
 * the number of records in the table can be larger, but we don't want a
 * whole lot of records per bucket or performance goes down.
 *
 * In a hash table allocated in shared memory, the directory cannot be
 * expanded because it must stay at a fixed address.  The directory size
 * should be selected using hash_select_dirsize (and you'd better have
 * a good idea of the maximum number of entries!).	For non-shared hash
 * tables, the initial directory size can be left at the default.
 */
#define DEF_SEGSIZE			   256
#define DEF_SEGSIZE_SHIFT	   8	/* must be log2(DEF_SEGSIZE) */
#define DEF_DIRSIZE			   256
#define DEF_FFACTOR			   1	/* default fill factor */


/*
 * HASHELEMENT is the private part of a hashtable entry.  The caller's data
 * follows the HASHELEMENT structure (on a MAXALIGN'd boundary).  The hash key
 * is expected to be at the start of the caller's hash entry data structure.
 */
typedef struct HASHELEMENT
{
	struct HASHELEMENT *link;	/* link to next entry in same bucket */
	uint32	hashvalue;			/* hash function result for this entry */
} HASHELEMENT;

/* A hash bucket is a linked list of HASHELEMENTs */
typedef HASHELEMENT *HASHBUCKET;

/* A hash segment is an array of bucket headers */
typedef HASHBUCKET *HASHSEGMENT;

/* Header structure for a hash table --- contains all changeable info */
typedef struct HASHHDR
{
	long		dsize;			/* Directory Size */
	long		ssize;			/* Segment Size --- must be power of 2 */
	int			sshift;			/* Segment shift = log2(ssize) */
	uint32		max_bucket;		/* ID of Maximum bucket in use */
	uint32		high_mask;		/* Mask to modulo into entire table */
	uint32		low_mask;		/* Mask to modulo into lower half of table */
	long		ffactor;		/* Fill factor */
	long		nentries;		/* Number of entries in hash table */
	long		nsegs;			/* Number of allocated segments */
	Size		keysize;		/* hash key length in bytes */
	Size		entrysize;		/* total user element size in bytes */
	long		max_dsize;		/* 'dsize' limit if directory is fixed
								 * size */
	HASHELEMENT *freeList;		/* linked list of free elements */
#ifdef HASH_STATISTICS
	long		accesses;
	long		collisions;
#endif
} HASHHDR;

/*
 * Top control structure for a hashtable --- need not be shared, since
 * no fields change at runtime
 */
typedef struct HTAB
{
	HASHHDR    *hctl;			/* shared control information */
	HASHSEGMENT *dir;			/* directory of segment starts */
	HashValueFunc hash;			/* hash function */
	HashCompareFunc match;		/* key comparison function */
	HashAllocFunc alloc;		/* memory allocator */
	MemoryContext hcxt;			/* memory context if default allocator
								 * used */
	char	   *tabname;		/* table name (for error messages) */
	bool		isshared;		/* true if table is in shared memory */
	HashCopyFunc keycopy;		/* key copying function */
} HTAB;

/* Parameter data structure for hash_create */
/* Only those fields indicated by hash_flags need be set */
typedef struct HASHCTL
{
	long		ssize;			/* Segment Size */
	long		dsize;			/* (initial) Directory Size */
	long		max_dsize;		/* limit to dsize if directory size is
								 * limited */
	long		ffactor;		/* Fill factor */
	Size		keysize;		/* hash key length in bytes */
	Size		entrysize;		/* total user element size in bytes */
	HashValueFunc hash;			/* hash function */
	HashCompareFunc match;		/* key comparison function */
	HashAllocFunc alloc;		/* memory allocator */
	HASHSEGMENT *dir;			/* directory of segment starts */
	HASHHDR    *hctl;			/* location of header in shared mem */
	MemoryContext hcxt;			/* memory context to use for allocations */
} HASHCTL;

/* Flags to indicate which parameters are supplied */
#define HASH_SEGMENT	0x002	/* Set segment size */
#define HASH_DIRSIZE	0x004	/* Set directory size */
#define HASH_FFACTOR	0x008	/* Set fill factor */
#define HASH_FUNCTION	0x010	/* Set user defined hash function */
#define HASH_ELEM		0x020	/* Set key/entry size */
#define HASH_SHARED_MEM 0x040	/* Set shared mem const */
#define HASH_ATTACH		0x080	/* Do not initialize hctl */
#define HASH_ALLOC		0x100	/* Set memory allocator */
#define HASH_CONTEXT	0x200	/* Set explicit memory context */
#define HASH_COMPARE	0x400	/* Set user defined comparison function */


/* max_dsize value to indicate expansible directory */
#define NO_MAX_DSIZE			(-1)
/* number of hash elements allocated at once */
#define HASHELEMENT_ALLOC_INCR	(32)

/* hash_search operations */
typedef enum
{
	HASH_FIND,
	HASH_ENTER,
	HASH_REMOVE,
	HASH_FIND_SAVE,
	HASH_REMOVE_SAVED
} HASHACTION;

/* hash_seq status (should be considered an opaque type by callers) */
typedef struct
{
	HTAB	   *hashp;
	uint32		curBucket;		/* index of current bucket */
	HASHELEMENT *curEntry;		/* current entry in bucket */
} HASH_SEQ_STATUS;

/*
 * prototypes for functions in dynahash.c
 */
extern HTAB *hash_create(const char *tabname, long nelem,
			HASHCTL *info, int flags);
extern void hash_destroy(HTAB *hashp);
extern void hash_stats(const char *where, HTAB *hashp);
extern void *hash_search(HTAB *hashp, const void *keyPtr, HASHACTION action,
			bool *foundPtr);
extern void hash_seq_init(HASH_SEQ_STATUS *status, HTAB *hashp);
extern void *hash_seq_search(HASH_SEQ_STATUS *status);
extern long hash_estimate_size(long num_entries, Size entrysize);
extern long hash_select_dirsize(long num_entries);

/*
 * prototypes for functions in hashfn.c
 */
extern uint32 string_hash(const void *key, Size keysize);
extern uint32 tag_hash(const void *key, Size keysize);

#endif   /* HSEARCH_H */
