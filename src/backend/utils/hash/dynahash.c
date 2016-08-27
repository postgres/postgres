/*-------------------------------------------------------------------------
 *
 * dynahash.c
 *	  dynamic hash tables
 *
 * dynahash.c supports both local-to-a-backend hash tables and hash tables in
 * shared memory.  For shared hash tables, it is the caller's responsibility
 * to provide appropriate access interlocking.  The simplest convention is
 * that a single LWLock protects the whole hash table.  Searches (HASH_FIND or
 * hash_seq_search) need only shared lock, but any update requires exclusive
 * lock.  For heavily-used shared tables, the single-lock approach creates a
 * concurrency bottleneck, so we also support "partitioned" locking wherein
 * there are multiple LWLocks guarding distinct subsets of the table.  To use
 * a hash table in partitioned mode, the HASH_PARTITION flag must be given
 * to hash_create.  This prevents any attempt to split buckets on-the-fly.
 * Therefore, each hash bucket chain operates independently, and no fields
 * of the hash header change after init except nentries and freeList.
 * A partitioned table uses spinlocks to guard changes of those fields.
 * This lets any subset of the hash buckets be treated as a separately
 * lockable partition.  We expect callers to use the low-order bits of a
 * lookup key's hash value as a partition number --- this will work because
 * of the way calc_bucket() maps hash values to bucket numbers.
 *
 * For hash tables in shared memory, the memory allocator function should
 * match malloc's semantics of returning NULL on failure.  For hash tables
 * in local memory, we typically use palloc() which will throw error on
 * failure.  The code in this file has to cope with both cases.
 *
 * dynahash.c provides support for these types of lookup keys:
 *
 * 1. Null-terminated C strings (truncated if necessary to fit in keysize),
 * compared as though by strcmp().  This is the default behavior.
 *
 * 2. Arbitrary binary data of size keysize, compared as though by memcmp().
 * (Caller must ensure there are no undefined padding bits in the keys!)
 * This is selected by specifying HASH_BLOBS flag to hash_create.
 *
 * 3. More complex key behavior can be selected by specifying user-supplied
 * hashing, comparison, and/or key-copying functions.  At least a hashing
 * function must be supplied; comparison defaults to memcmp() and key copying
 * to memcpy() when a user-defined hashing function is selected.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/hash/dynahash.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * Original comments:
 *
 * Dynamic hashing, after CACM April 1988 pp 446-457, by Per-Ake Larson.
 * Coded into C, with minor code improvements, and with hsearch(3) interface,
 * by ejp@ausmelb.oz, Jul 26, 1988: 13:16;
 * also, hcreate/hdestroy routines added to simulate hsearch(3).
 *
 * These routines simulate hsearch(3) and family, with the important
 * difference that the hash table is dynamic - can grow indefinitely
 * beyond its original size (as supplied to hcreate()).
 *
 * Performance appears to be comparable to that of hsearch(3).
 * The 'source-code' options referred to in hsearch(3)'s 'man' page
 * are not implemented; otherwise functionality is identical.
 *
 * Compilation controls:
 * HASH_DEBUG controls some informative traces, mainly for debugging.
 * HASH_STATISTICS causes HashAccesses and HashCollisions to be maintained;
 * when combined with HASH_DEBUG, these are displayed by hdestroy().
 *
 * Problems & fixes to ejp@ausmelb.oz. WARNING: relies on pre-processor
 * concatenation property, in probably unnecessary code 'optimisation'.
 *
 * Modified margo@postgres.berkeley.edu February 1990
 *		added multiple table interface
 * Modified by sullivan@postgres.berkeley.edu April 1990
 *		changed ctl structure for shared memory
 */

#include "postgres.h"

#include <limits.h>

#include "access/xact.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/dynahash.h"
#include "utils/memutils.h"


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
 * expanded because it must stay at a fixed address.  The directory size
 * should be selected using hash_select_dirsize (and you'd better have
 * a good idea of the maximum number of entries!).  For non-shared hash
 * tables, the initial directory size can be left at the default.
 */
#define DEF_SEGSIZE			   256
#define DEF_SEGSIZE_SHIFT	   8	/* must be log2(DEF_SEGSIZE) */
#define DEF_DIRSIZE			   256
#define DEF_FFACTOR			   1	/* default fill factor */

/* Number of freelists to be used for a partitioned hash table. */
#define NUM_FREELISTS			32

/* A hash bucket is a linked list of HASHELEMENTs */
typedef HASHELEMENT *HASHBUCKET;

/* A hash segment is an array of bucket headers */
typedef HASHBUCKET *HASHSEGMENT;

/*
 * Using array of FreeListData instead of separate arrays of mutexes, nentries
 * and freeLists prevents, at least partially, sharing one cache line between
 * different mutexes (see below).
 */
typedef struct
{
	slock_t		mutex;			/* spinlock */
	long		nentries;		/* number of entries */
	HASHELEMENT *freeList;		/* list of free elements */
} FreeListData;

/*
 * Header structure for a hash table --- contains all changeable info
 *
 * In a shared-memory hash table, the HASHHDR is in shared memory, while
 * each backend has a local HTAB struct.  For a non-shared table, there isn't
 * any functional difference between HASHHDR and HTAB, but we separate them
 * anyway to share code between shared and non-shared tables.
 */
struct HASHHDR
{
	/*
	 * The freelist can become a point of contention on high-concurrency hash
	 * tables, so we use an array of freelist, each with its own mutex and
	 * nentries count, instead of just a single one.
	 *
	 * If hash table is not partitioned only freeList[0] is used and spinlocks
	 * are not used at all.
	 */
	FreeListData freeList[NUM_FREELISTS];

	/* These fields can change, but not in a partitioned table */
	/* Also, dsize can't change in a shared table, even if unpartitioned */
	long		dsize;			/* directory size */
	long		nsegs;			/* number of allocated segments (<= dsize) */
	uint32		max_bucket;		/* ID of maximum bucket in use */
	uint32		high_mask;		/* mask to modulo into entire table */
	uint32		low_mask;		/* mask to modulo into lower half of table */

	/* These fields are fixed at hashtable creation */
	Size		keysize;		/* hash key length in bytes */
	Size		entrysize;		/* total user element size in bytes */
	long		num_partitions; /* # partitions (must be power of 2), or 0 */
	long		ffactor;		/* target fill factor */
	long		max_dsize;		/* 'dsize' limit if directory is fixed size */
	long		ssize;			/* segment size --- must be power of 2 */
	int			sshift;			/* segment shift = log2(ssize) */
	int			nelem_alloc;	/* number of entries to allocate at once */

#ifdef HASH_STATISTICS

	/*
	 * Count statistics here.  NB: stats code doesn't bother with mutex, so
	 * counts could be corrupted a bit in a partitioned table.
	 */
	long		accesses;
	long		collisions;
#endif
};

#define IS_PARTITIONED(hctl)  ((hctl)->num_partitions != 0)

#define FREELIST_IDX(hctl, hashcode) \
	(IS_PARTITIONED(hctl) ? hashcode % NUM_FREELISTS : 0)

/*
 * Top control structure for a hashtable --- in a shared table, each backend
 * has its own copy (OK since no fields change at runtime)
 */
struct HTAB
{
	HASHHDR    *hctl;			/* => shared control information */
	HASHSEGMENT *dir;			/* directory of segment starts */
	HashValueFunc hash;			/* hash function */
	HashCompareFunc match;		/* key comparison function */
	HashCopyFunc keycopy;		/* key copying function */
	HashAllocFunc alloc;		/* memory allocator */
	MemoryContext hcxt;			/* memory context if default allocator used */
	char	   *tabname;		/* table name (for error messages) */
	bool		isshared;		/* true if table is in shared memory */
	bool		isfixed;		/* if true, don't enlarge */

	/* freezing a shared table isn't allowed, so we can keep state here */
	bool		frozen;			/* true = no more inserts allowed */

	/* We keep local copies of these fixed values to reduce contention */
	Size		keysize;		/* hash key length in bytes */
	long		ssize;			/* segment size --- must be power of 2 */
	int			sshift;			/* segment shift = log2(ssize) */
};

/*
 * Key (also entry) part of a HASHELEMENT
 */
#define ELEMENTKEY(helem)  (((char *)(helem)) + MAXALIGN(sizeof(HASHELEMENT)))

/*
 * Obtain element pointer given pointer to key
 */
#define ELEMENT_FROM_KEY(key)  \
	((HASHELEMENT *) (((char *) (key)) - MAXALIGN(sizeof(HASHELEMENT))))

/*
 * Fast MOD arithmetic, assuming that y is a power of 2 !
 */
#define MOD(x,y)			   ((x) & ((y)-1))

#if HASH_STATISTICS
static long hash_accesses,
			hash_collisions,
			hash_expansions;
#endif

/*
 * Private function prototypes
 */
static void *DynaHashAlloc(Size size);
static HASHSEGMENT seg_alloc(HTAB *hashp);
static bool element_alloc(HTAB *hashp, int nelem, int freelist_idx);
static bool dir_realloc(HTAB *hashp);
static bool expand_table(HTAB *hashp);
static HASHBUCKET get_hash_entry(HTAB *hashp, int freelist_idx);
static void hdefault(HTAB *hashp);
static int	choose_nelem_alloc(Size entrysize);
static bool init_htab(HTAB *hashp, long nelem);
static void hash_corrupted(HTAB *hashp);
static long next_pow2_long(long num);
static int	next_pow2_int(long num);
static void register_seq_scan(HTAB *hashp);
static void deregister_seq_scan(HTAB *hashp);
static bool has_seq_scans(HTAB *hashp);


/*
 * memory allocation support
 */
static MemoryContext CurrentDynaHashCxt = NULL;

static void *
DynaHashAlloc(Size size)
{
	Assert(MemoryContextIsValid(CurrentDynaHashCxt));
	return MemoryContextAlloc(CurrentDynaHashCxt, size);
}


/*
 * HashCompareFunc for string keys
 *
 * Because we copy keys with strlcpy(), they will be truncated at keysize-1
 * bytes, so we can only compare that many ... hence strncmp is almost but
 * not quite the right thing.
 */
static int
string_compare(const char *key1, const char *key2, Size keysize)
{
	return strncmp(key1, key2, keysize - 1);
}


/************************** CREATE ROUTINES **********************/

/*
 * hash_create -- create a new dynamic hash table
 *
 *	tabname: a name for the table (for debugging purposes)
 *	nelem: maximum number of elements expected
 *	*info: additional table parameters, as indicated by flags
 *	flags: bitmask indicating which parameters to take from *info
 *
 * Note: for a shared-memory hashtable, nelem needs to be a pretty good
 * estimate, since we can't expand the table on the fly.  But an unshared
 * hashtable can be expanded on-the-fly, so it's better for nelem to be
 * on the small side and let the table grow if it's exceeded.  An overly
 * large nelem will penalize hash_seq_search speed without buying much.
 */
HTAB *
hash_create(const char *tabname, long nelem, HASHCTL *info, int flags)
{
	HTAB	   *hashp;
	HASHHDR    *hctl;

	/*
	 * For shared hash tables, we have a local hash header (HTAB struct) that
	 * we allocate in TopMemoryContext; all else is in shared memory.
	 *
	 * For non-shared hash tables, everything including the hash header is in
	 * a memory context created specially for the hash table --- this makes
	 * hash_destroy very simple.  The memory context is made a child of either
	 * a context specified by the caller, or TopMemoryContext if nothing is
	 * specified.
	 */
	if (flags & HASH_SHARED_MEM)
	{
		/* Set up to allocate the hash header */
		CurrentDynaHashCxt = TopMemoryContext;
	}
	else
	{
		/* Create the hash table's private memory context */
		if (flags & HASH_CONTEXT)
			CurrentDynaHashCxt = info->hcxt;
		else
			CurrentDynaHashCxt = TopMemoryContext;
		CurrentDynaHashCxt = AllocSetContextCreate(CurrentDynaHashCxt,
												   tabname,
												   ALLOCSET_DEFAULT_SIZES);
	}

	/* Initialize the hash header, plus a copy of the table name */
	hashp = (HTAB *) DynaHashAlloc(sizeof(HTAB) + strlen(tabname) +1);
	MemSet(hashp, 0, sizeof(HTAB));

	hashp->tabname = (char *) (hashp + 1);
	strcpy(hashp->tabname, tabname);

	/*
	 * Select the appropriate hash function (see comments at head of file).
	 */
	if (flags & HASH_FUNCTION)
		hashp->hash = info->hash;
	else if (flags & HASH_BLOBS)
	{
		/* We can optimize hashing for common key sizes */
		Assert(flags & HASH_ELEM);
		if (info->keysize == sizeof(uint32))
			hashp->hash = uint32_hash;
		else
			hashp->hash = tag_hash;
	}
	else
		hashp->hash = string_hash;		/* default hash function */

	/*
	 * If you don't specify a match function, it defaults to string_compare if
	 * you used string_hash (either explicitly or by default) and to memcmp
	 * otherwise.
	 *
	 * Note: explicitly specifying string_hash is deprecated, because this
	 * might not work for callers in loadable modules on some platforms due to
	 * referencing a trampoline instead of the string_hash function proper.
	 * Just let it default, eh?
	 */
	if (flags & HASH_COMPARE)
		hashp->match = info->match;
	else if (hashp->hash == string_hash)
		hashp->match = (HashCompareFunc) string_compare;
	else
		hashp->match = memcmp;

	/*
	 * Similarly, the key-copying function defaults to strlcpy or memcpy.
	 */
	if (flags & HASH_KEYCOPY)
		hashp->keycopy = info->keycopy;
	else if (hashp->hash == string_hash)
		hashp->keycopy = (HashCopyFunc) strlcpy;
	else
		hashp->keycopy = memcpy;

	/* And select the entry allocation function, too. */
	if (flags & HASH_ALLOC)
		hashp->alloc = info->alloc;
	else
		hashp->alloc = DynaHashAlloc;

	if (flags & HASH_SHARED_MEM)
	{
		/*
		 * ctl structure and directory are preallocated for shared memory
		 * tables.  Note that HASH_DIRSIZE and HASH_ALLOC had better be set as
		 * well.
		 */
		hashp->hctl = info->hctl;
		hashp->dir = (HASHSEGMENT *) (((char *) info->hctl) + sizeof(HASHHDR));
		hashp->hcxt = NULL;
		hashp->isshared = true;

		/* hash table already exists, we're just attaching to it */
		if (flags & HASH_ATTACH)
		{
			/* make local copies of some heavily-used values */
			hctl = hashp->hctl;
			hashp->keysize = hctl->keysize;
			hashp->ssize = hctl->ssize;
			hashp->sshift = hctl->sshift;

			return hashp;
		}
	}
	else
	{
		/* setup hash table defaults */
		hashp->hctl = NULL;
		hashp->dir = NULL;
		hashp->hcxt = CurrentDynaHashCxt;
		hashp->isshared = false;
	}

	if (!hashp->hctl)
	{
		hashp->hctl = (HASHHDR *) hashp->alloc(sizeof(HASHHDR));
		if (!hashp->hctl)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
	}

	hashp->frozen = false;

	hdefault(hashp);

	hctl = hashp->hctl;

	if (flags & HASH_PARTITION)
	{
		/* Doesn't make sense to partition a local hash table */
		Assert(flags & HASH_SHARED_MEM);

		/*
		 * The number of partitions had better be a power of 2. Also, it must
		 * be less than INT_MAX (see init_htab()), so call the int version of
		 * next_pow2.
		 */
		Assert(info->num_partitions == next_pow2_int(info->num_partitions));

		hctl->num_partitions = info->num_partitions;
	}

	if (flags & HASH_SEGMENT)
	{
		hctl->ssize = info->ssize;
		hctl->sshift = my_log2(info->ssize);
		/* ssize had better be a power of 2 */
		Assert(hctl->ssize == (1L << hctl->sshift));
	}
	if (flags & HASH_FFACTOR)
		hctl->ffactor = info->ffactor;

	/*
	 * SHM hash tables have fixed directory size passed by the caller.
	 */
	if (flags & HASH_DIRSIZE)
	{
		hctl->max_dsize = info->max_dsize;
		hctl->dsize = info->dsize;
	}

	/*
	 * hash table now allocates space for key and data but you have to say how
	 * much space to allocate
	 */
	if (flags & HASH_ELEM)
	{
		Assert(info->entrysize >= info->keysize);
		hctl->keysize = info->keysize;
		hctl->entrysize = info->entrysize;
	}

	/* make local copies of heavily-used constant fields */
	hashp->keysize = hctl->keysize;
	hashp->ssize = hctl->ssize;
	hashp->sshift = hctl->sshift;

	/* Build the hash directory structure */
	if (!init_htab(hashp, nelem))
		elog(ERROR, "failed to initialize hash table \"%s\"", hashp->tabname);

	/*
	 * For a shared hash table, preallocate the requested number of elements.
	 * This reduces problems with run-time out-of-shared-memory conditions.
	 *
	 * For a non-shared hash table, preallocate the requested number of
	 * elements if it's less than our chosen nelem_alloc.  This avoids wasting
	 * space if the caller correctly estimates a small table size.
	 */
	if ((flags & HASH_SHARED_MEM) ||
		nelem < hctl->nelem_alloc)
	{
		int			i,
					freelist_partitions,
					nelem_alloc,
					nelem_alloc_first;

		/*
		 * If hash table is partitioned all freeLists have equal number of
		 * elements. Otherwise only freeList[0] is used.
		 */
		if (IS_PARTITIONED(hashp->hctl))
			freelist_partitions = NUM_FREELISTS;
		else
			freelist_partitions = 1;

		nelem_alloc = nelem / freelist_partitions;
		if (nelem_alloc == 0)
			nelem_alloc = 1;

		/* Make sure all memory will be used */
		if (nelem_alloc * freelist_partitions < nelem)
			nelem_alloc_first =
				nelem - nelem_alloc * (freelist_partitions - 1);
		else
			nelem_alloc_first = nelem_alloc;

		for (i = 0; i < freelist_partitions; i++)
		{
			int			temp = (i == 0) ? nelem_alloc_first : nelem_alloc;

			if (!element_alloc(hashp, temp, i))
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
		}
	}

	if (flags & HASH_FIXED_SIZE)
		hashp->isfixed = true;
	return hashp;
}

/*
 * Set default HASHHDR parameters.
 */
static void
hdefault(HTAB *hashp)
{
	HASHHDR    *hctl = hashp->hctl;

	MemSet(hctl, 0, sizeof(HASHHDR));

	hctl->dsize = DEF_DIRSIZE;
	hctl->nsegs = 0;

	/* rather pointless defaults for key & entry size */
	hctl->keysize = sizeof(char *);
	hctl->entrysize = 2 * sizeof(char *);

	hctl->num_partitions = 0;	/* not partitioned */

	hctl->ffactor = DEF_FFACTOR;

	/* table has no fixed maximum size */
	hctl->max_dsize = NO_MAX_DSIZE;

	hctl->ssize = DEF_SEGSIZE;
	hctl->sshift = DEF_SEGSIZE_SHIFT;

#ifdef HASH_STATISTICS
	hctl->accesses = hctl->collisions = 0;
#endif
}

/*
 * Given the user-specified entry size, choose nelem_alloc, ie, how many
 * elements to add to the hash table when we need more.
 */
static int
choose_nelem_alloc(Size entrysize)
{
	int			nelem_alloc;
	Size		elementSize;
	Size		allocSize;

	/* Each element has a HASHELEMENT header plus user data. */
	/* NB: this had better match element_alloc() */
	elementSize = MAXALIGN(sizeof(HASHELEMENT)) + MAXALIGN(entrysize);

	/*
	 * The idea here is to choose nelem_alloc at least 32, but round up so
	 * that the allocation request will be a power of 2 or just less. This
	 * makes little difference for hash tables in shared memory, but for hash
	 * tables managed by palloc, the allocation request will be rounded up to
	 * a power of 2 anyway.  If we fail to take this into account, we'll waste
	 * as much as half the allocated space.
	 */
	allocSize = 32 * 4;			/* assume elementSize at least 8 */
	do
	{
		allocSize <<= 1;
		nelem_alloc = allocSize / elementSize;
	} while (nelem_alloc < 32);

	return nelem_alloc;
}

/*
 * Compute derived fields of hctl and build the initial directory/segment
 * arrays
 */
static bool
init_htab(HTAB *hashp, long nelem)
{
	HASHHDR    *hctl = hashp->hctl;
	HASHSEGMENT *segp;
	int			nbuckets;
	int			nsegs;
	int			i;

	/*
	 * initialize mutex if it's a partitioned table
	 */
	if (IS_PARTITIONED(hctl))
		for (i = 0; i < NUM_FREELISTS; i++)
			SpinLockInit(&(hctl->freeList[i].mutex));

	/*
	 * Divide number of elements by the fill factor to determine a desired
	 * number of buckets.  Allocate space for the next greater power of two
	 * number of buckets
	 */
	nbuckets = next_pow2_int((nelem - 1) / hctl->ffactor + 1);

	/*
	 * In a partitioned table, nbuckets must be at least equal to
	 * num_partitions; were it less, keys with apparently different partition
	 * numbers would map to the same bucket, breaking partition independence.
	 * (Normally nbuckets will be much bigger; this is just a safety check.)
	 */
	while (nbuckets < hctl->num_partitions)
		nbuckets <<= 1;

	hctl->max_bucket = hctl->low_mask = nbuckets - 1;
	hctl->high_mask = (nbuckets << 1) - 1;

	/*
	 * Figure number of directory segments needed, round up to a power of 2
	 */
	nsegs = (nbuckets - 1) / hctl->ssize + 1;
	nsegs = next_pow2_int(nsegs);

	/*
	 * Make sure directory is big enough. If pre-allocated directory is too
	 * small, choke (caller screwed up).
	 */
	if (nsegs > hctl->dsize)
	{
		if (!(hashp->dir))
			hctl->dsize = nsegs;
		else
			return false;
	}

	/* Allocate a directory */
	if (!(hashp->dir))
	{
		CurrentDynaHashCxt = hashp->hcxt;
		hashp->dir = (HASHSEGMENT *)
			hashp->alloc(hctl->dsize * sizeof(HASHSEGMENT));
		if (!hashp->dir)
			return false;
	}

	/* Allocate initial segments */
	for (segp = hashp->dir; hctl->nsegs < nsegs; hctl->nsegs++, segp++)
	{
		*segp = seg_alloc(hashp);
		if (*segp == NULL)
			return false;
	}

	/* Choose number of entries to allocate at a time */
	hctl->nelem_alloc = choose_nelem_alloc(hctl->entrysize);

#if HASH_DEBUG
	fprintf(stderr, "init_htab:\n%s%p\n%s%ld\n%s%ld\n%s%d\n%s%ld\n%s%u\n%s%x\n%s%x\n%s%ld\n%s%ld\n",
			"TABLE POINTER   ", hashp,
			"DIRECTORY SIZE  ", hctl->dsize,
			"SEGMENT SIZE    ", hctl->ssize,
			"SEGMENT SHIFT   ", hctl->sshift,
			"FILL FACTOR     ", hctl->ffactor,
			"MAX BUCKET      ", hctl->max_bucket,
			"HIGH MASK       ", hctl->high_mask,
			"LOW  MASK       ", hctl->low_mask,
			"NSEGS           ", hctl->nsegs,
			"NENTRIES        ", hash_get_num_entries(hctl));
#endif
	return true;
}

/*
 * Estimate the space needed for a hashtable containing the given number
 * of entries of given size.
 * NOTE: this is used to estimate the footprint of hashtables in shared
 * memory; therefore it does not count HTAB which is in local memory.
 * NB: assumes that all hash structure parameters have default values!
 */
Size
hash_estimate_size(long num_entries, Size entrysize)
{
	Size		size;
	long		nBuckets,
				nSegments,
				nDirEntries,
				nElementAllocs,
				elementSize,
				elementAllocCnt;

	/* estimate number of buckets wanted */
	nBuckets = next_pow2_long((num_entries - 1) / DEF_FFACTOR + 1);
	/* # of segments needed for nBuckets */
	nSegments = next_pow2_long((nBuckets - 1) / DEF_SEGSIZE + 1);
	/* directory entries */
	nDirEntries = DEF_DIRSIZE;
	while (nDirEntries < nSegments)
		nDirEntries <<= 1;		/* dir_alloc doubles dsize at each call */

	/* fixed control info */
	size = MAXALIGN(sizeof(HASHHDR));	/* but not HTAB, per above */
	/* directory */
	size = add_size(size, mul_size(nDirEntries, sizeof(HASHSEGMENT)));
	/* segments */
	size = add_size(size, mul_size(nSegments,
								MAXALIGN(DEF_SEGSIZE * sizeof(HASHBUCKET))));
	/* elements --- allocated in groups of choose_nelem_alloc() entries */
	elementAllocCnt = choose_nelem_alloc(entrysize);
	nElementAllocs = (num_entries - 1) / elementAllocCnt + 1;
	elementSize = MAXALIGN(sizeof(HASHELEMENT)) + MAXALIGN(entrysize);
	size = add_size(size,
					mul_size(nElementAllocs,
							 mul_size(elementAllocCnt, elementSize)));

	return size;
}

/*
 * Select an appropriate directory size for a hashtable with the given
 * maximum number of entries.
 * This is only needed for hashtables in shared memory, whose directories
 * cannot be expanded dynamically.
 * NB: assumes that all hash structure parameters have default values!
 *
 * XXX this had better agree with the behavior of init_htab()...
 */
long
hash_select_dirsize(long num_entries)
{
	long		nBuckets,
				nSegments,
				nDirEntries;

	/* estimate number of buckets wanted */
	nBuckets = next_pow2_long((num_entries - 1) / DEF_FFACTOR + 1);
	/* # of segments needed for nBuckets */
	nSegments = next_pow2_long((nBuckets - 1) / DEF_SEGSIZE + 1);
	/* directory entries */
	nDirEntries = DEF_DIRSIZE;
	while (nDirEntries < nSegments)
		nDirEntries <<= 1;		/* dir_alloc doubles dsize at each call */

	return nDirEntries;
}

/*
 * Compute the required initial memory allocation for a shared-memory
 * hashtable with the given parameters.  We need space for the HASHHDR
 * and for the (non expansible) directory.
 */
Size
hash_get_shared_size(HASHCTL *info, int flags)
{
	Assert(flags & HASH_DIRSIZE);
	Assert(info->dsize == info->max_dsize);
	return sizeof(HASHHDR) + info->dsize * sizeof(HASHSEGMENT);
}


/********************** DESTROY ROUTINES ************************/

void
hash_destroy(HTAB *hashp)
{
	if (hashp != NULL)
	{
		/* allocation method must be one we know how to free, too */
		Assert(hashp->alloc == DynaHashAlloc);
		/* so this hashtable must have it's own context */
		Assert(hashp->hcxt != NULL);

		hash_stats("destroy", hashp);

		/*
		 * Free everything by destroying the hash table's memory context.
		 */
		MemoryContextDelete(hashp->hcxt);
	}
}

void
hash_stats(const char *where, HTAB *hashp)
{
#if HASH_STATISTICS
	fprintf(stderr, "%s: this HTAB -- accesses %ld collisions %ld\n",
			where, hashp->hctl->accesses, hashp->hctl->collisions);

	fprintf(stderr, "hash_stats: entries %ld keysize %ld maxp %u segmentcount %ld\n",
			hash_get_num_entries(hashp), (long) hashp->hctl->keysize,
			hashp->hctl->max_bucket, hashp->hctl->nsegs);
	fprintf(stderr, "%s: total accesses %ld total collisions %ld\n",
			where, hash_accesses, hash_collisions);
	fprintf(stderr, "hash_stats: total expansions %ld\n",
			hash_expansions);
#endif
}

/*******************************SEARCH ROUTINES *****************************/


/*
 * get_hash_value -- exported routine to calculate a key's hash value
 *
 * We export this because for partitioned tables, callers need to compute
 * the partition number (from the low-order bits of the hash value) before
 * searching.
 */
uint32
get_hash_value(HTAB *hashp, const void *keyPtr)
{
	return hashp->hash(keyPtr, hashp->keysize);
}

/* Convert a hash value to a bucket number */
static inline uint32
calc_bucket(HASHHDR *hctl, uint32 hash_val)
{
	uint32		bucket;

	bucket = hash_val & hctl->high_mask;
	if (bucket > hctl->max_bucket)
		bucket = bucket & hctl->low_mask;

	return bucket;
}

/*
 * hash_search -- look up key in table and perform action
 * hash_search_with_hash_value -- same, with key's hash value already computed
 *
 * action is one of:
 *		HASH_FIND: look up key in table
 *		HASH_ENTER: look up key in table, creating entry if not present
 *		HASH_ENTER_NULL: same, but return NULL if out of memory
 *		HASH_REMOVE: look up key in table, remove entry if present
 *
 * Return value is a pointer to the element found/entered/removed if any,
 * or NULL if no match was found.  (NB: in the case of the REMOVE action,
 * the result is a dangling pointer that shouldn't be dereferenced!)
 *
 * HASH_ENTER will normally ereport a generic "out of memory" error if
 * it is unable to create a new entry.  The HASH_ENTER_NULL operation is
 * the same except it will return NULL if out of memory.  Note that
 * HASH_ENTER_NULL cannot be used with the default palloc-based allocator,
 * since palloc internally ereports on out-of-memory.
 *
 * If foundPtr isn't NULL, then *foundPtr is set TRUE if we found an
 * existing entry in the table, FALSE otherwise.  This is needed in the
 * HASH_ENTER case, but is redundant with the return value otherwise.
 *
 * For hash_search_with_hash_value, the hashvalue parameter must have been
 * calculated with get_hash_value().
 */
void *
hash_search(HTAB *hashp,
			const void *keyPtr,
			HASHACTION action,
			bool *foundPtr)
{
	return hash_search_with_hash_value(hashp,
									   keyPtr,
									   hashp->hash(keyPtr, hashp->keysize),
									   action,
									   foundPtr);
}

void *
hash_search_with_hash_value(HTAB *hashp,
							const void *keyPtr,
							uint32 hashvalue,
							HASHACTION action,
							bool *foundPtr)
{
	HASHHDR    *hctl = hashp->hctl;
	Size		keysize;
	uint32		bucket;
	long		segment_num;
	long		segment_ndx;
	HASHSEGMENT segp;
	HASHBUCKET	currBucket;
	HASHBUCKET *prevBucketPtr;
	HashCompareFunc match;
	int			freelist_idx = FREELIST_IDX(hctl, hashvalue);

#if HASH_STATISTICS
	hash_accesses++;
	hctl->accesses++;
#endif

	/*
	 * If inserting, check if it is time to split a bucket.
	 *
	 * NOTE: failure to expand table is not a fatal error, it just means we
	 * have to run at higher fill factor than we wanted.  However, if we're
	 * using the palloc allocator then it will throw error anyway on
	 * out-of-memory, so we must do this before modifying the table.
	 */
	if (action == HASH_ENTER || action == HASH_ENTER_NULL)
	{
		/*
		 * Can't split if running in partitioned mode, nor if frozen, nor if
		 * table is the subject of any active hash_seq_search scans.  Strange
		 * order of these tests is to try to check cheaper conditions first.
		 */
		if (!IS_PARTITIONED(hctl) && !hashp->frozen &&
			hctl->freeList[0].nentries / (long) (hctl->max_bucket + 1) >= hctl->ffactor &&
			!has_seq_scans(hashp))
			(void) expand_table(hashp);
	}

	/*
	 * Do the initial lookup
	 */
	bucket = calc_bucket(hctl, hashvalue);

	segment_num = bucket >> hashp->sshift;
	segment_ndx = MOD(bucket, hashp->ssize);

	segp = hashp->dir[segment_num];

	if (segp == NULL)
		hash_corrupted(hashp);

	prevBucketPtr = &segp[segment_ndx];
	currBucket = *prevBucketPtr;

	/*
	 * Follow collision chain looking for matching key
	 */
	match = hashp->match;		/* save one fetch in inner loop */
	keysize = hashp->keysize;	/* ditto */

	while (currBucket != NULL)
	{
		if (currBucket->hashvalue == hashvalue &&
			match(ELEMENTKEY(currBucket), keyPtr, keysize) == 0)
			break;
		prevBucketPtr = &(currBucket->link);
		currBucket = *prevBucketPtr;
#if HASH_STATISTICS
		hash_collisions++;
		hctl->collisions++;
#endif
	}

	if (foundPtr)
		*foundPtr = (bool) (currBucket != NULL);

	/*
	 * OK, now what?
	 */
	switch (action)
	{
		case HASH_FIND:
			if (currBucket != NULL)
				return (void *) ELEMENTKEY(currBucket);
			return NULL;

		case HASH_REMOVE:
			if (currBucket != NULL)
			{
				/* if partitioned, must lock to touch nentries and freeList */
				if (IS_PARTITIONED(hctl))
					SpinLockAcquire(&(hctl->freeList[freelist_idx].mutex));

				Assert(hctl->freeList[freelist_idx].nentries > 0);
				hctl->freeList[freelist_idx].nentries--;

				/* remove record from hash bucket's chain. */
				*prevBucketPtr = currBucket->link;

				/* add the record to the freelist for this table.  */
				currBucket->link = hctl->freeList[freelist_idx].freeList;
				hctl->freeList[freelist_idx].freeList = currBucket;

				if (IS_PARTITIONED(hctl))
					SpinLockRelease(&hctl->freeList[freelist_idx].mutex);

				/*
				 * better hope the caller is synchronizing access to this
				 * element, because someone else is going to reuse it the next
				 * time something is added to the table
				 */
				return (void *) ELEMENTKEY(currBucket);
			}
			return NULL;

		case HASH_ENTER_NULL:
			/* ENTER_NULL does not work with palloc-based allocator */
			Assert(hashp->alloc != DynaHashAlloc);
			/* FALL THRU */

		case HASH_ENTER:
			/* Return existing element if found, else create one */
			if (currBucket != NULL)
				return (void *) ELEMENTKEY(currBucket);

			/* disallow inserts if frozen */
			if (hashp->frozen)
				elog(ERROR, "cannot insert into frozen hashtable \"%s\"",
					 hashp->tabname);

			currBucket = get_hash_entry(hashp, freelist_idx);
			if (currBucket == NULL)
			{
				/* out of memory */
				if (action == HASH_ENTER_NULL)
					return NULL;
				/* report a generic message */
				if (hashp->isshared)
					ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							 errmsg("out of shared memory")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							 errmsg("out of memory")));
			}

			/* link into hashbucket chain */
			*prevBucketPtr = currBucket;
			currBucket->link = NULL;

			/* copy key into record */
			currBucket->hashvalue = hashvalue;
			hashp->keycopy(ELEMENTKEY(currBucket), keyPtr, keysize);

			/*
			 * Caller is expected to fill the data field on return.  DO NOT
			 * insert any code that could possibly throw error here, as doing
			 * so would leave the table entry incomplete and hence corrupt the
			 * caller's data structure.
			 */

			return (void *) ELEMENTKEY(currBucket);
	}

	elog(ERROR, "unrecognized hash action code: %d", (int) action);

	return NULL;				/* keep compiler quiet */
}

/*
 * hash_update_hash_key -- change the hash key of an existing table entry
 *
 * This is equivalent to removing the entry, making a new entry, and copying
 * over its data, except that the entry never goes to the table's freelist.
 * Therefore this cannot suffer an out-of-memory failure, even if there are
 * other processes operating in other partitions of the hashtable.
 *
 * Returns TRUE if successful, FALSE if the requested new hash key is already
 * present.  Throws error if the specified entry pointer isn't actually a
 * table member.
 *
 * NB: currently, there is no special case for old and new hash keys being
 * identical, which means we'll report FALSE for that situation.  This is
 * preferable for existing uses.
 *
 * NB: for a partitioned hashtable, caller must hold lock on both relevant
 * partitions, if the new hash key would belong to a different partition.
 */
bool
hash_update_hash_key(HTAB *hashp,
					 void *existingEntry,
					 const void *newKeyPtr)
{
	HASHELEMENT *existingElement = ELEMENT_FROM_KEY(existingEntry);
	HASHHDR    *hctl = hashp->hctl;
	uint32		newhashvalue;
	Size		keysize;
	uint32		bucket;
	uint32		newbucket;
	long		segment_num;
	long		segment_ndx;
	HASHSEGMENT segp;
	HASHBUCKET	currBucket;
	HASHBUCKET *prevBucketPtr;
	HASHBUCKET *oldPrevPtr;
	HashCompareFunc match;

#if HASH_STATISTICS
	hash_accesses++;
	hctl->accesses++;
#endif

	/* disallow updates if frozen */
	if (hashp->frozen)
		elog(ERROR, "cannot update in frozen hashtable \"%s\"",
			 hashp->tabname);

	/*
	 * Lookup the existing element using its saved hash value.  We need to do
	 * this to be able to unlink it from its hash chain, but as a side benefit
	 * we can verify the validity of the passed existingEntry pointer.
	 */
	bucket = calc_bucket(hctl, existingElement->hashvalue);

	segment_num = bucket >> hashp->sshift;
	segment_ndx = MOD(bucket, hashp->ssize);

	segp = hashp->dir[segment_num];

	if (segp == NULL)
		hash_corrupted(hashp);

	prevBucketPtr = &segp[segment_ndx];
	currBucket = *prevBucketPtr;

	while (currBucket != NULL)
	{
		if (currBucket == existingElement)
			break;
		prevBucketPtr = &(currBucket->link);
		currBucket = *prevBucketPtr;
	}

	if (currBucket == NULL)
		elog(ERROR, "hash_update_hash_key argument is not in hashtable \"%s\"",
			 hashp->tabname);

	oldPrevPtr = prevBucketPtr;

	/*
	 * Now perform the equivalent of a HASH_ENTER operation to locate the hash
	 * chain we want to put the entry into.
	 */
	newhashvalue = hashp->hash(newKeyPtr, hashp->keysize);

	newbucket = calc_bucket(hctl, newhashvalue);

	segment_num = newbucket >> hashp->sshift;
	segment_ndx = MOD(newbucket, hashp->ssize);

	segp = hashp->dir[segment_num];

	if (segp == NULL)
		hash_corrupted(hashp);

	prevBucketPtr = &segp[segment_ndx];
	currBucket = *prevBucketPtr;

	/*
	 * Follow collision chain looking for matching key
	 */
	match = hashp->match;		/* save one fetch in inner loop */
	keysize = hashp->keysize;	/* ditto */

	while (currBucket != NULL)
	{
		if (currBucket->hashvalue == newhashvalue &&
			match(ELEMENTKEY(currBucket), newKeyPtr, keysize) == 0)
			break;
		prevBucketPtr = &(currBucket->link);
		currBucket = *prevBucketPtr;
#if HASH_STATISTICS
		hash_collisions++;
		hctl->collisions++;
#endif
	}

	if (currBucket != NULL)
		return false;			/* collision with an existing entry */

	currBucket = existingElement;

	/*
	 * If old and new hash values belong to the same bucket, we need not
	 * change any chain links, and indeed should not since this simplistic
	 * update will corrupt the list if currBucket is the last element.  (We
	 * cannot fall out earlier, however, since we need to scan the bucket to
	 * check for duplicate keys.)
	 */
	if (bucket != newbucket)
	{
		/* OK to remove record from old hash bucket's chain. */
		*oldPrevPtr = currBucket->link;

		/* link into new hashbucket chain */
		*prevBucketPtr = currBucket;
		currBucket->link = NULL;
	}

	/* copy new key into record */
	currBucket->hashvalue = newhashvalue;
	hashp->keycopy(ELEMENTKEY(currBucket), newKeyPtr, keysize);

	/* rest of record is untouched */

	return true;
}

/*
 * create a new entry if possible
 */
static HASHBUCKET
get_hash_entry(HTAB *hashp, int freelist_idx)
{
	HASHHDR    *hctl = hashp->hctl;
	HASHBUCKET	newElement;
	int			borrow_from_idx;

	for (;;)
	{
		/* if partitioned, must lock to touch nentries and freeList */
		if (IS_PARTITIONED(hctl))
			SpinLockAcquire(&hctl->freeList[freelist_idx].mutex);

		/* try to get an entry from the freelist */
		newElement = hctl->freeList[freelist_idx].freeList;

		if (newElement != NULL)
			break;

		if (IS_PARTITIONED(hctl))
			SpinLockRelease(&hctl->freeList[freelist_idx].mutex);

		/* no free elements.  allocate another chunk of buckets */
		if (!element_alloc(hashp, hctl->nelem_alloc, freelist_idx))
		{
			if (!IS_PARTITIONED(hctl))
				return NULL;	/* out of memory */

			/* try to borrow element from another partition */
			borrow_from_idx = freelist_idx;
			for (;;)
			{
				borrow_from_idx = (borrow_from_idx + 1) % NUM_FREELISTS;
				if (borrow_from_idx == freelist_idx)
					break;

				SpinLockAcquire(&(hctl->freeList[borrow_from_idx].mutex));
				newElement = hctl->freeList[borrow_from_idx].freeList;

				if (newElement != NULL)
				{
					hctl->freeList[borrow_from_idx].freeList = newElement->link;
					SpinLockRelease(&(hctl->freeList[borrow_from_idx].mutex));

					SpinLockAcquire(&hctl->freeList[freelist_idx].mutex);
					hctl->freeList[freelist_idx].nentries++;
					SpinLockRelease(&hctl->freeList[freelist_idx].mutex);

					break;
				}

				SpinLockRelease(&(hctl->freeList[borrow_from_idx].mutex));
			}

			return newElement;
		}
	}

	/* remove entry from freelist, bump nentries */
	hctl->freeList[freelist_idx].freeList = newElement->link;
	hctl->freeList[freelist_idx].nentries++;

	if (IS_PARTITIONED(hctl))
		SpinLockRelease(&hctl->freeList[freelist_idx].mutex);

	return newElement;
}

/*
 * hash_get_num_entries -- get the number of entries in a hashtable
 */
long
hash_get_num_entries(HTAB *hashp)
{
	int			i;
	long		sum = hashp->hctl->freeList[0].nentries;

	/*
	 * We currently don't bother with the mutex; it's only sensible to call
	 * this function if you've got lock on all partitions of the table.
	 */

	if (!IS_PARTITIONED(hashp->hctl))
		return sum;

	for (i = 1; i < NUM_FREELISTS; i++)
		sum += hashp->hctl->freeList[i].nentries;

	return sum;
}

/*
 * hash_seq_init/_search/_term
 *			Sequentially search through hash table and return
 *			all the elements one by one, return NULL when no more.
 *
 * hash_seq_term should be called if and only if the scan is abandoned before
 * completion; if hash_seq_search returns NULL then it has already done the
 * end-of-scan cleanup.
 *
 * NOTE: caller may delete the returned element before continuing the scan.
 * However, deleting any other element while the scan is in progress is
 * UNDEFINED (it might be the one that curIndex is pointing at!).  Also,
 * if elements are added to the table while the scan is in progress, it is
 * unspecified whether they will be visited by the scan or not.
 *
 * NOTE: it is possible to use hash_seq_init/hash_seq_search without any
 * worry about hash_seq_term cleanup, if the hashtable is first locked against
 * further insertions by calling hash_freeze.  This is used by nodeAgg.c,
 * wherein it is inconvenient to track whether a scan is still open, and
 * there's no possibility of further insertions after readout has begun.
 *
 * NOTE: to use this with a partitioned hashtable, caller had better hold
 * at least shared lock on all partitions of the table throughout the scan!
 * We can cope with insertions or deletions by our own backend, but *not*
 * with concurrent insertions or deletions by another.
 */
void
hash_seq_init(HASH_SEQ_STATUS *status, HTAB *hashp)
{
	status->hashp = hashp;
	status->curBucket = 0;
	status->curEntry = NULL;
	if (!hashp->frozen)
		register_seq_scan(hashp);
}

void *
hash_seq_search(HASH_SEQ_STATUS *status)
{
	HTAB	   *hashp;
	HASHHDR    *hctl;
	uint32		max_bucket;
	long		ssize;
	long		segment_num;
	long		segment_ndx;
	HASHSEGMENT segp;
	uint32		curBucket;
	HASHELEMENT *curElem;

	if ((curElem = status->curEntry) != NULL)
	{
		/* Continuing scan of curBucket... */
		status->curEntry = curElem->link;
		if (status->curEntry == NULL)	/* end of this bucket */
			++status->curBucket;
		return (void *) ELEMENTKEY(curElem);
	}

	/*
	 * Search for next nonempty bucket starting at curBucket.
	 */
	curBucket = status->curBucket;
	hashp = status->hashp;
	hctl = hashp->hctl;
	ssize = hashp->ssize;
	max_bucket = hctl->max_bucket;

	if (curBucket > max_bucket)
	{
		hash_seq_term(status);
		return NULL;			/* search is done */
	}

	/*
	 * first find the right segment in the table directory.
	 */
	segment_num = curBucket >> hashp->sshift;
	segment_ndx = MOD(curBucket, ssize);

	segp = hashp->dir[segment_num];

	/*
	 * Pick up the first item in this bucket's chain.  If chain is not empty
	 * we can begin searching it.  Otherwise we have to advance to find the
	 * next nonempty bucket.  We try to optimize that case since searching a
	 * near-empty hashtable has to iterate this loop a lot.
	 */
	while ((curElem = segp[segment_ndx]) == NULL)
	{
		/* empty bucket, advance to next */
		if (++curBucket > max_bucket)
		{
			status->curBucket = curBucket;
			hash_seq_term(status);
			return NULL;		/* search is done */
		}
		if (++segment_ndx >= ssize)
		{
			segment_num++;
			segment_ndx = 0;
			segp = hashp->dir[segment_num];
		}
	}

	/* Begin scan of curBucket... */
	status->curEntry = curElem->link;
	if (status->curEntry == NULL)		/* end of this bucket */
		++curBucket;
	status->curBucket = curBucket;
	return (void *) ELEMENTKEY(curElem);
}

void
hash_seq_term(HASH_SEQ_STATUS *status)
{
	if (!status->hashp->frozen)
		deregister_seq_scan(status->hashp);
}

/*
 * hash_freeze
 *			Freeze a hashtable against future insertions (deletions are
 *			still allowed)
 *
 * The reason for doing this is that by preventing any more bucket splits,
 * we no longer need to worry about registering hash_seq_search scans,
 * and thus caller need not be careful about ensuring hash_seq_term gets
 * called at the right times.
 *
 * Multiple calls to hash_freeze() are allowed, but you can't freeze a table
 * with active scans (since hash_seq_term would then do the wrong thing).
 */
void
hash_freeze(HTAB *hashp)
{
	if (hashp->isshared)
		elog(ERROR, "cannot freeze shared hashtable \"%s\"", hashp->tabname);
	if (!hashp->frozen && has_seq_scans(hashp))
		elog(ERROR, "cannot freeze hashtable \"%s\" because it has active scans",
			 hashp->tabname);
	hashp->frozen = true;
}


/********************************* UTILITIES ************************/

/*
 * Expand the table by adding one more hash bucket.
 */
static bool
expand_table(HTAB *hashp)
{
	HASHHDR    *hctl = hashp->hctl;
	HASHSEGMENT old_seg,
				new_seg;
	long		old_bucket,
				new_bucket;
	long		new_segnum,
				new_segndx;
	long		old_segnum,
				old_segndx;
	HASHBUCKET *oldlink,
			   *newlink;
	HASHBUCKET	currElement,
				nextElement;

	Assert(!IS_PARTITIONED(hctl));

#ifdef HASH_STATISTICS
	hash_expansions++;
#endif

	new_bucket = hctl->max_bucket + 1;
	new_segnum = new_bucket >> hashp->sshift;
	new_segndx = MOD(new_bucket, hashp->ssize);

	if (new_segnum >= hctl->nsegs)
	{
		/* Allocate new segment if necessary -- could fail if dir full */
		if (new_segnum >= hctl->dsize)
			if (!dir_realloc(hashp))
				return false;
		if (!(hashp->dir[new_segnum] = seg_alloc(hashp)))
			return false;
		hctl->nsegs++;
	}

	/* OK, we created a new bucket */
	hctl->max_bucket++;

	/*
	 * *Before* changing masks, find old bucket corresponding to same hash
	 * values; values in that bucket may need to be relocated to new bucket.
	 * Note that new_bucket is certainly larger than low_mask at this point,
	 * so we can skip the first step of the regular hash mask calc.
	 */
	old_bucket = (new_bucket & hctl->low_mask);

	/*
	 * If we crossed a power of 2, readjust masks.
	 */
	if ((uint32) new_bucket > hctl->high_mask)
	{
		hctl->low_mask = hctl->high_mask;
		hctl->high_mask = (uint32) new_bucket | hctl->low_mask;
	}

	/*
	 * Relocate records to the new bucket.  NOTE: because of the way the hash
	 * masking is done in calc_bucket, only one old bucket can need to be
	 * split at this point.  With a different way of reducing the hash value,
	 * that might not be true!
	 */
	old_segnum = old_bucket >> hashp->sshift;
	old_segndx = MOD(old_bucket, hashp->ssize);

	old_seg = hashp->dir[old_segnum];
	new_seg = hashp->dir[new_segnum];

	oldlink = &old_seg[old_segndx];
	newlink = &new_seg[new_segndx];

	for (currElement = *oldlink;
		 currElement != NULL;
		 currElement = nextElement)
	{
		nextElement = currElement->link;
		if ((long) calc_bucket(hctl, currElement->hashvalue) == old_bucket)
		{
			*oldlink = currElement;
			oldlink = &currElement->link;
		}
		else
		{
			*newlink = currElement;
			newlink = &currElement->link;
		}
	}
	/* don't forget to terminate the rebuilt hash chains... */
	*oldlink = NULL;
	*newlink = NULL;

	return true;
}


static bool
dir_realloc(HTAB *hashp)
{
	HASHSEGMENT *p;
	HASHSEGMENT *old_p;
	long		new_dsize;
	long		old_dirsize;
	long		new_dirsize;

	if (hashp->hctl->max_dsize != NO_MAX_DSIZE)
		return false;

	/* Reallocate directory */
	new_dsize = hashp->hctl->dsize << 1;
	old_dirsize = hashp->hctl->dsize * sizeof(HASHSEGMENT);
	new_dirsize = new_dsize * sizeof(HASHSEGMENT);

	old_p = hashp->dir;
	CurrentDynaHashCxt = hashp->hcxt;
	p = (HASHSEGMENT *) hashp->alloc((Size) new_dirsize);

	if (p != NULL)
	{
		memcpy(p, old_p, old_dirsize);
		MemSet(((char *) p) + old_dirsize, 0, new_dirsize - old_dirsize);
		hashp->dir = p;
		hashp->hctl->dsize = new_dsize;

		/* XXX assume the allocator is palloc, so we know how to free */
		Assert(hashp->alloc == DynaHashAlloc);
		pfree(old_p);

		return true;
	}

	return false;
}


static HASHSEGMENT
seg_alloc(HTAB *hashp)
{
	HASHSEGMENT segp;

	CurrentDynaHashCxt = hashp->hcxt;
	segp = (HASHSEGMENT) hashp->alloc(sizeof(HASHBUCKET) * hashp->ssize);

	if (!segp)
		return NULL;

	MemSet(segp, 0, sizeof(HASHBUCKET) * hashp->ssize);

	return segp;
}

/*
 * allocate some new elements and link them into the indicated free list
 */
static bool
element_alloc(HTAB *hashp, int nelem, int freelist_idx)
{
	HASHHDR    *hctl = hashp->hctl;
	Size		elementSize;
	HASHELEMENT *firstElement;
	HASHELEMENT *tmpElement;
	HASHELEMENT *prevElement;
	int			i;

	if (hashp->isfixed)
		return false;

	/* Each element has a HASHELEMENT header plus user data. */
	elementSize = MAXALIGN(sizeof(HASHELEMENT)) + MAXALIGN(hctl->entrysize);

	CurrentDynaHashCxt = hashp->hcxt;
	firstElement = (HASHELEMENT *) hashp->alloc(nelem * elementSize);

	if (!firstElement)
		return false;

	/* prepare to link all the new entries into the freelist */
	prevElement = NULL;
	tmpElement = firstElement;
	for (i = 0; i < nelem; i++)
	{
		tmpElement->link = prevElement;
		prevElement = tmpElement;
		tmpElement = (HASHELEMENT *) (((char *) tmpElement) + elementSize);
	}

	/* if partitioned, must lock to touch freeList */
	if (IS_PARTITIONED(hctl))
		SpinLockAcquire(&hctl->freeList[freelist_idx].mutex);

	/* freelist could be nonempty if two backends did this concurrently */
	firstElement->link = hctl->freeList[freelist_idx].freeList;
	hctl->freeList[freelist_idx].freeList = prevElement;

	if (IS_PARTITIONED(hctl))
		SpinLockRelease(&hctl->freeList[freelist_idx].mutex);

	return true;
}

/* complain when we have detected a corrupted hashtable */
static void
hash_corrupted(HTAB *hashp)
{
	/*
	 * If the corruption is in a shared hashtable, we'd better force a
	 * systemwide restart.  Otherwise, just shut down this one backend.
	 */
	if (hashp->isshared)
		elog(PANIC, "hash table \"%s\" corrupted", hashp->tabname);
	else
		elog(FATAL, "hash table \"%s\" corrupted", hashp->tabname);
}

/* calculate ceil(log base 2) of num */
int
my_log2(long num)
{
	int			i;
	long		limit;

	/* guard against too-large input, which would put us into infinite loop */
	if (num > LONG_MAX / 2)
		num = LONG_MAX / 2;

	for (i = 0, limit = 1; limit < num; i++, limit <<= 1)
		;
	return i;
}

/* calculate first power of 2 >= num, bounded to what will fit in a long */
static long
next_pow2_long(long num)
{
	/* my_log2's internal range check is sufficient */
	return 1L << my_log2(num);
}

/* calculate first power of 2 >= num, bounded to what will fit in an int */
static int
next_pow2_int(long num)
{
	if (num > INT_MAX / 2)
		num = INT_MAX / 2;
	return 1 << my_log2(num);
}


/************************* SEQ SCAN TRACKING ************************/

/*
 * We track active hash_seq_search scans here.  The need for this mechanism
 * comes from the fact that a scan will get confused if a bucket split occurs
 * while it's in progress: it might visit entries twice, or even miss some
 * entirely (if it's partway through the same bucket that splits).  Hence
 * we want to inhibit bucket splits if there are any active scans on the
 * table being inserted into.  This is a fairly rare case in current usage,
 * so just postponing the split until the next insertion seems sufficient.
 *
 * Given present usages of the function, only a few scans are likely to be
 * open concurrently; so a finite-size stack of open scans seems sufficient,
 * and we don't worry that linear search is too slow.  Note that we do
 * allow multiple scans of the same hashtable to be open concurrently.
 *
 * This mechanism can support concurrent scan and insertion in a shared
 * hashtable if it's the same backend doing both.  It would fail otherwise,
 * but locking reasons seem to preclude any such scenario anyway, so we don't
 * worry.
 *
 * This arrangement is reasonably robust if a transient hashtable is deleted
 * without notifying us.  The absolute worst case is we might inhibit splits
 * in another table created later at exactly the same address.  We will give
 * a warning at transaction end for reference leaks, so any bugs leading to
 * lack of notification should be easy to catch.
 */

#define MAX_SEQ_SCANS 100

static HTAB *seq_scan_tables[MAX_SEQ_SCANS];	/* tables being scanned */
static int	seq_scan_level[MAX_SEQ_SCANS];		/* subtransaction nest level */
static int	num_seq_scans = 0;


/* Register a table as having an active hash_seq_search scan */
static void
register_seq_scan(HTAB *hashp)
{
	if (num_seq_scans >= MAX_SEQ_SCANS)
		elog(ERROR, "too many active hash_seq_search scans, cannot start one on \"%s\"",
			 hashp->tabname);
	seq_scan_tables[num_seq_scans] = hashp;
	seq_scan_level[num_seq_scans] = GetCurrentTransactionNestLevel();
	num_seq_scans++;
}

/* Deregister an active scan */
static void
deregister_seq_scan(HTAB *hashp)
{
	int			i;

	/* Search backward since it's most likely at the stack top */
	for (i = num_seq_scans - 1; i >= 0; i--)
	{
		if (seq_scan_tables[i] == hashp)
		{
			seq_scan_tables[i] = seq_scan_tables[num_seq_scans - 1];
			seq_scan_level[i] = seq_scan_level[num_seq_scans - 1];
			num_seq_scans--;
			return;
		}
	}
	elog(ERROR, "no hash_seq_search scan for hash table \"%s\"",
		 hashp->tabname);
}

/* Check if a table has any active scan */
static bool
has_seq_scans(HTAB *hashp)
{
	int			i;

	for (i = 0; i < num_seq_scans; i++)
	{
		if (seq_scan_tables[i] == hashp)
			return true;
	}
	return false;
}

/* Clean up any open scans at end of transaction */
void
AtEOXact_HashTables(bool isCommit)
{
	/*
	 * During abort cleanup, open scans are expected; just silently clean 'em
	 * out.  An open scan at commit means someone forgot a hash_seq_term()
	 * call, so complain.
	 *
	 * Note: it's tempting to try to print the tabname here, but refrain for
	 * fear of touching deallocated memory.  This isn't a user-facing message
	 * anyway, so it needn't be pretty.
	 */
	if (isCommit)
	{
		int			i;

		for (i = 0; i < num_seq_scans; i++)
		{
			elog(WARNING, "leaked hash_seq_search scan for hash table %p",
				 seq_scan_tables[i]);
		}
	}
	num_seq_scans = 0;
}

/* Clean up any open scans at end of subtransaction */
void
AtEOSubXact_HashTables(bool isCommit, int nestDepth)
{
	int			i;

	/*
	 * Search backward to make cleanup easy.  Note we must check all entries,
	 * not only those at the end of the array, because deletion technique
	 * doesn't keep them in order.
	 */
	for (i = num_seq_scans - 1; i >= 0; i--)
	{
		if (seq_scan_level[i] >= nestDepth)
		{
			if (isCommit)
				elog(WARNING, "leaked hash_seq_search scan for hash table %p",
					 seq_scan_tables[i]);
			seq_scan_tables[i] = seq_scan_tables[num_seq_scans - 1];
			seq_scan_level[i] = seq_scan_level[num_seq_scans - 1];
			num_seq_scans--;
		}
	}
}
