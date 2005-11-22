/*-------------------------------------------------------------------------
 *
 * dynahash.c
 *	  dynamic hash tables
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/hash/dynahash.c,v 1.65.2.1 2005/11/22 18:23:24 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
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
 * DEBUG controls some informative traces, mainly for debugging.
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

#include "storage/shmem.h"
#include "utils/dynahash.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

/*
 * Key (also entry) part of a HASHELEMENT
 */
#define ELEMENTKEY(helem)  (((char *)(helem)) + MAXALIGN(sizeof(HASHELEMENT)))

/*
 * Fast MOD arithmetic, assuming that y is a power of 2 !
 */
#define MOD(x,y)			   ((x) & ((y)-1))

/*
 * Private function prototypes
 */
static void *DynaHashAlloc(Size size);
static HASHSEGMENT seg_alloc(HTAB *hashp);
static bool element_alloc(HTAB *hashp, int nelem);
static bool dir_realloc(HTAB *hashp);
static bool expand_table(HTAB *hashp);
static void hdefault(HTAB *hashp);
static bool init_htab(HTAB *hashp, long nelem);
static void hash_corrupted(HTAB *hashp);


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


#if HASH_STATISTICS
static long hash_accesses,
			hash_collisions,
			hash_expansions;
#endif


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
												   ALLOCSET_DEFAULT_MINSIZE,
												   ALLOCSET_DEFAULT_INITSIZE,
												   ALLOCSET_DEFAULT_MAXSIZE);
	}

	/* Initialize the hash header, plus a copy of the table name */
	hashp = (HTAB *) DynaHashAlloc(sizeof(HTAB) + strlen(tabname) +1);
	MemSet(hashp, 0, sizeof(HTAB));

	hashp->tabname = (char *) (hashp + 1);
	strcpy(hashp->tabname, tabname);

	if (flags & HASH_FUNCTION)
		hashp->hash = info->hash;
	else
		hashp->hash = string_hash;		/* default hash function */

	/*
	 * If you don't specify a match function, it defaults to strncmp() if you
	 * used string_hash (either explicitly or by default) and to memcmp()
	 * otherwise.  (Prior to PostgreSQL 7.4, memcmp() was always used.)
	 */
	if (flags & HASH_COMPARE)
		hashp->match = info->match;
	else if (hashp->hash == string_hash)
		hashp->match = (HashCompareFunc) strncmp;
	else
		hashp->match = memcmp;

	/*
	 * Similarly, the key-copying function defaults to strncpy() or memcpy().
	 */
	if (flags & HASH_KEYCOPY)
		hashp->keycopy = info->keycopy;
	else if (hashp->hash == string_hash)
		hashp->keycopy = (HashCopyFunc) strncpy;
	else
		hashp->keycopy = memcpy;

	if (flags & HASH_ALLOC)
		hashp->alloc = info->alloc;
	else
		hashp->alloc = DynaHashAlloc;

	if (flags & HASH_SHARED_MEM)
	{
		/*
		 * ctl structure is preallocated for shared memory tables. Note that
		 * HASH_DIRSIZE and HASH_ALLOC had better be set as well.
		 */
		hashp->hctl = info->hctl;
		hashp->dir = info->dir;
		hashp->hcxt = NULL;
		hashp->isshared = true;

		/* hash table already exists, we're just attaching to it */
		if (flags & HASH_ATTACH)
			return hashp;
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

	hdefault(hashp);

	hctl = hashp->hctl;
#ifdef HASH_STATISTICS
	hctl->accesses = hctl->collisions = 0;
#endif

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

	/* Build the hash directory structure */
	if (!init_htab(hashp, nelem))
	{
		hash_destroy(hashp);
		elog(ERROR, "failed to initialize hash table");
	}

	/*
	 * For a shared hash table, preallocate the requested number of elements.
	 * This reduces problems with run-time out-of-shared-memory conditions.
	 */
	if (flags & HASH_SHARED_MEM)
	{
		if (!element_alloc(hashp, (int) nelem))
		{
			hash_destroy(hashp);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}
	}

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

	hctl->ssize = DEF_SEGSIZE;
	hctl->sshift = DEF_SEGSIZE_SHIFT;
	hctl->dsize = DEF_DIRSIZE;
	hctl->ffactor = DEF_FFACTOR;
	hctl->nentries = 0;
	hctl->nsegs = 0;

	/* rather pointless defaults for key & entry size */
	hctl->keysize = sizeof(char *);
	hctl->entrysize = 2 * sizeof(char *);

	/* table has no fixed maximum size */
	hctl->max_dsize = NO_MAX_DSIZE;

	/* garbage collection for HASH_REMOVE */
	hctl->freeList = NULL;
}


static bool
init_htab(HTAB *hashp, long nelem)
{
	HASHHDR    *hctl = hashp->hctl;
	HASHSEGMENT *segp;
	long		lnbuckets;
	int			nbuckets;
	int			nsegs;

	/*
	 * Divide number of elements by the fill factor to determine a desired
	 * number of buckets.  Allocate space for the next greater power of two
	 * number of buckets
	 */
	lnbuckets = (nelem - 1) / hctl->ffactor + 1;

	nbuckets = 1 << my_log2(lnbuckets);

	hctl->max_bucket = hctl->low_mask = nbuckets - 1;
	hctl->high_mask = (nbuckets << 1) - 1;

	/*
	 * Figure number of directory segments needed, round up to a power of 2
	 */
	nsegs = (nbuckets - 1) / hctl->ssize + 1;
	nsegs = 1 << my_log2(nsegs);

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
	hctl->nelem_alloc = (int) Min(nelem, HASHELEMENT_ALLOC_MAX);
	hctl->nelem_alloc = Max(hctl->nelem_alloc, 1);

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
			"NENTRIES        ", hctl->nentries);
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
	nBuckets = 1L << my_log2((num_entries - 1) / DEF_FFACTOR + 1);
	/* # of segments needed for nBuckets */
	nSegments = 1L << my_log2((nBuckets - 1) / DEF_SEGSIZE + 1);
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
	/* elements --- allocated in groups of up to HASHELEMENT_ALLOC_MAX */
	elementSize = MAXALIGN(sizeof(HASHELEMENT)) + MAXALIGN(entrysize);
	elementAllocCnt = Min(num_entries, HASHELEMENT_ALLOC_MAX);
	elementAllocCnt = Max(elementAllocCnt, 1);
	nElementAllocs = (num_entries - 1) / elementAllocCnt + 1;
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
	nBuckets = 1L << my_log2((num_entries - 1) / DEF_FFACTOR + 1);
	/* # of segments needed for nBuckets */
	nSegments = 1L << my_log2((nBuckets - 1) / DEF_SEGSIZE + 1);
	/* directory entries */
	nDirEntries = DEF_DIRSIZE;
	while (nDirEntries < nSegments)
		nDirEntries <<= 1;		/* dir_alloc doubles dsize at each call */

	return nDirEntries;
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
			hashp->hctl->nentries, hashp->hctl->keysize,
			hashp->hctl->max_bucket, hashp->hctl->nsegs);
	fprintf(stderr, "%s: total accesses %ld total collisions %ld\n",
			where, hash_accesses, hash_collisions);
	fprintf(stderr, "hash_stats: total expansions %ld\n",
			hash_expansions);
#endif
}

/*******************************SEARCH ROUTINES *****************************/


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

/*----------
 * hash_search -- look up key in table and perform action
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
 * it is unable to create a new entry.	The HASH_ENTER_NULL operation is
 * the same except it will return NULL if out of memory.  Note that
 * HASH_ENTER_NULL cannot be used with the default palloc-based allocator,
 * since palloc internally ereports on out-of-memory.
 *
 * If foundPtr isn't NULL, then *foundPtr is set TRUE if we found an
 * existing entry in the table, FALSE otherwise.  This is needed in the
 * HASH_ENTER case, but is redundant with the return value otherwise.
 *----------
 */
void *
hash_search(HTAB *hashp,
			const void *keyPtr,
			HASHACTION action,
			bool *foundPtr)
{
	HASHHDR    *hctl = hashp->hctl;
	Size		keysize = hctl->keysize;
	uint32		hashvalue;
	uint32		bucket;
	long		segment_num;
	long		segment_ndx;
	HASHSEGMENT segp;
	HASHBUCKET	currBucket;
	HASHBUCKET *prevBucketPtr;
	HashCompareFunc match;

#if HASH_STATISTICS
	hash_accesses++;
	hctl->accesses++;
#endif

	/*
	 * Do the initial lookup
	 */
	hashvalue = hashp->hash(keyPtr, keysize);
	bucket = calc_bucket(hctl, hashvalue);

	segment_num = bucket >> hctl->sshift;
	segment_ndx = MOD(bucket, hctl->ssize);

	segp = hashp->dir[segment_num];

	if (segp == NULL)
		hash_corrupted(hashp);

	prevBucketPtr = &segp[segment_ndx];
	currBucket = *prevBucketPtr;

	/*
	 * Follow collision chain looking for matching key
	 */
	match = hashp->match;		/* save one fetch in inner loop */

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
				Assert(hctl->nentries > 0);
				hctl->nentries--;

				/* remove record from hash bucket's chain. */
				*prevBucketPtr = currBucket->link;

				/* add the record to the freelist for this table.  */
				currBucket->link = hctl->freeList;
				hctl->freeList = currBucket;

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

			/* get the next free element */
			currBucket = hctl->freeList;
			if (currBucket == NULL)
			{
				/* no free elements.  allocate another chunk of buckets */
				if (!element_alloc(hashp, hctl->nelem_alloc))
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
				currBucket = hctl->freeList;
				Assert(currBucket != NULL);
			}

			hctl->freeList = currBucket->link;

			/* link into hashbucket chain */
			*prevBucketPtr = currBucket;
			currBucket->link = NULL;

			/* copy key into record */
			currBucket->hashvalue = hashvalue;
			hashp->keycopy(ELEMENTKEY(currBucket), keyPtr, keysize);

			/* caller is expected to fill the data field on return */

			/* Check if it is time to split the segment */
			if (++hctl->nentries / (long) (hctl->max_bucket + 1) >= hctl->ffactor)
			{
				/*
				 * NOTE: failure to expand table is not a fatal error, it just
				 * means we have to run at higher fill factor than we wanted.
				 */
				expand_table(hashp);
			}

			return (void *) ELEMENTKEY(currBucket);
	}

	elog(ERROR, "unrecognized hash action code: %d", (int) action);

	return NULL;				/* keep compiler quiet */
}

/*
 * hash_seq_init/_search
 *			Sequentially search through hash table and return
 *			all the elements one by one, return NULL when no more.
 *
 * NOTE: caller may delete the returned element before continuing the scan.
 * However, deleting any other element while the scan is in progress is
 * UNDEFINED (it might be the one that curIndex is pointing at!).  Also,
 * if elements are added to the table while the scan is in progress, it is
 * unspecified whether they will be visited by the scan or not.
 */
void
hash_seq_init(HASH_SEQ_STATUS *status, HTAB *hashp)
{
	status->hashp = hashp;
	status->curBucket = 0;
	status->curEntry = NULL;
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
	ssize = hctl->ssize;
	max_bucket = hctl->max_bucket;

	if (curBucket > max_bucket)
		return NULL;			/* search is done */

	/*
	 * first find the right segment in the table directory.
	 */
	segment_num = curBucket >> hctl->sshift;
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

#ifdef HASH_STATISTICS
	hash_expansions++;
#endif

	new_bucket = hctl->max_bucket + 1;
	new_segnum = new_bucket >> hctl->sshift;
	new_segndx = MOD(new_bucket, hctl->ssize);

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
	 * Relocate records to the new bucket.	NOTE: because of the way the hash
	 * masking is done in calc_bucket, only one old bucket can need to be
	 * split at this point.  With a different way of reducing the hash value,
	 * that might not be true!
	 */
	old_segnum = old_bucket >> hctl->sshift;
	old_segndx = MOD(old_bucket, hctl->ssize);

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
	segp = (HASHSEGMENT) hashp->alloc(sizeof(HASHBUCKET) * hashp->hctl->ssize);

	if (!segp)
		return NULL;

	MemSet(segp, 0, sizeof(HASHBUCKET) * hashp->hctl->ssize);

	return segp;
}

/*
 * allocate some new elements and link them into the free list
 */
static bool
element_alloc(HTAB *hashp, int nelem)
{
	HASHHDR    *hctl = hashp->hctl;
	Size		elementSize;
	HASHELEMENT *tmpElement;
	int			i;

	/* Each element has a HASHELEMENT header plus user data. */
	elementSize = MAXALIGN(sizeof(HASHELEMENT)) + MAXALIGN(hctl->entrysize);

	CurrentDynaHashCxt = hashp->hcxt;
	tmpElement = (HASHELEMENT *)
		hashp->alloc(nelem * elementSize);

	if (!tmpElement)
		return false;

	/* link all the new entries into the freelist */
	for (i = 0; i < nelem; i++)
	{
		tmpElement->link = hctl->freeList;
		hctl->freeList = tmpElement;
		tmpElement = (HASHELEMENT *) (((char *) tmpElement) + elementSize);
	}

	return true;
}

/* complain when we have detected a corrupted hashtable */
static void
hash_corrupted(HTAB *hashp)
{
	/*
	 * If the corruption is in a shared hashtable, we'd better force a
	 * systemwide restart.	Otherwise, just shut down this one backend.
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

	for (i = 0, limit = 1; limit < num; i++, limit <<= 1)
		;
	return i;
}
