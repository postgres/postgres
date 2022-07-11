/*-------------------------------------------------------------------------
 *
 * dshash.c
 *	  Concurrent hash tables backed by dynamic shared memory areas.
 *
 * This is an open hashing hash table, with a linked list at each table
 * entry.  It supports dynamic resizing, as required to prevent the linked
 * lists from growing too long on average.  Currently, only growing is
 * supported: the hash table never becomes smaller.
 *
 * To deal with concurrency, it has a fixed size set of partitions, each of
 * which is independently locked.  Each bucket maps to a partition; so insert,
 * find and iterate operations normally only acquire one lock.  Therefore,
 * good concurrency is achieved whenever such operations don't collide at the
 * lock partition level.  However, when a resize operation begins, all
 * partition locks must be acquired simultaneously for a brief period.  This
 * is only expected to happen a small number of times until a stable size is
 * found, since growth is geometric.
 *
 * Future versions may support iterators and incremental resizing; for now
 * the implementation is minimalist.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/lib/dshash.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "lib/dshash.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "utils/dsa.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

/*
 * An item in the hash table.  This wraps the user's entry object in an
 * envelop that holds a pointer back to the bucket and a pointer to the next
 * item in the bucket.
 */
struct dshash_table_item
{
	/* The next item in the same bucket. */
	dsa_pointer next;
	/* The hashed key, to avoid having to recompute it. */
	dshash_hash hash;
	/* The user's entry object follows here.  See ENTRY_FROM_ITEM(item). */
};

/*
 * The number of partitions for locking purposes.  This is set to match
 * NUM_BUFFER_PARTITIONS for now, on the basis that whatever's good enough for
 * the buffer pool must be good enough for any other purpose.  This could
 * become a runtime parameter in future.
 */
#define DSHASH_NUM_PARTITIONS_LOG2 7
#define DSHASH_NUM_PARTITIONS (1 << DSHASH_NUM_PARTITIONS_LOG2)

/* A magic value used to identify our hash tables. */
#define DSHASH_MAGIC 0x75ff6a20

/*
 * Tracking information for each lock partition.  Initially, each partition
 * corresponds to one bucket, but each time the hash table grows, the buckets
 * covered by each partition split so the number of buckets covered doubles.
 *
 * We might want to add padding here so that each partition is on a different
 * cache line, but doing so would bloat this structure considerably.
 */
typedef struct dshash_partition
{
	LWLock		lock;			/* Protects all buckets in this partition. */
	size_t		count;			/* # of items in this partition's buckets */
} dshash_partition;

/*
 * The head object for a hash table.  This will be stored in dynamic shared
 * memory.
 */
typedef struct dshash_table_control
{
	dshash_table_handle handle;
	uint32		magic;
	dshash_partition partitions[DSHASH_NUM_PARTITIONS];
	int			lwlock_tranche_id;

	/*
	 * The following members are written to only when ALL partitions locks are
	 * held.  They can be read when any one partition lock is held.
	 */

	/* Number of buckets expressed as power of 2 (8 = 256 buckets). */
	size_t		size_log2;		/* log2(number of buckets) */
	dsa_pointer buckets;		/* current bucket array */
} dshash_table_control;

/*
 * Per-backend state for a dynamic hash table.
 */
struct dshash_table
{
	dsa_area   *area;			/* Backing dynamic shared memory area. */
	dshash_parameters params;	/* Parameters. */
	void	   *arg;			/* User-supplied data pointer. */
	dshash_table_control *control;	/* Control object in DSM. */
	dsa_pointer *buckets;		/* Current bucket pointers in DSM. */
	size_t		size_log2;		/* log2(number of buckets) */
};

/* Given a pointer to an item, find the entry (user data) it holds. */
#define ENTRY_FROM_ITEM(item) \
	((char *)(item) + MAXALIGN(sizeof(dshash_table_item)))

/* Given a pointer to an entry, find the item that holds it. */
#define ITEM_FROM_ENTRY(entry)											\
	((dshash_table_item *)((char *)(entry) -							\
							 MAXALIGN(sizeof(dshash_table_item))))

/* How many resize operations (bucket splits) have there been? */
#define NUM_SPLITS(size_log2)					\
	(size_log2 - DSHASH_NUM_PARTITIONS_LOG2)

/* How many buckets are there in each partition at a given size? */
#define BUCKETS_PER_PARTITION(size_log2)		\
	(((size_t) 1) << NUM_SPLITS(size_log2))

/* Max entries before we need to grow.  Half + quarter = 75% load factor. */
#define MAX_COUNT_PER_PARTITION(hash_table)				\
	(BUCKETS_PER_PARTITION(hash_table->size_log2) / 2 + \
	 BUCKETS_PER_PARTITION(hash_table->size_log2) / 4)

/* Choose partition based on the highest order bits of the hash. */
#define PARTITION_FOR_HASH(hash)										\
	(hash >> ((sizeof(dshash_hash) * CHAR_BIT) - DSHASH_NUM_PARTITIONS_LOG2))

/*
 * Find the bucket index for a given hash and table size.  Each time the table
 * doubles in size, the appropriate bucket for a given hash value doubles and
 * possibly adds one, depending on the newly revealed bit, so that all buckets
 * are split.
 */
#define BUCKET_INDEX_FOR_HASH_AND_SIZE(hash, size_log2)		\
	(hash >> ((sizeof(dshash_hash) * CHAR_BIT) - (size_log2)))

/* The index of the first bucket in a given partition. */
#define BUCKET_INDEX_FOR_PARTITION(partition, size_log2)	\
	((partition) << NUM_SPLITS(size_log2))

/* The head of the active bucket for a given hash value (lvalue). */
#define BUCKET_FOR_HASH(hash_table, hash)								\
	(hash_table->buckets[												\
		BUCKET_INDEX_FOR_HASH_AND_SIZE(hash,							\
									   hash_table->size_log2)])

static void delete_item(dshash_table *hash_table,
						dshash_table_item *item);
static void resize(dshash_table *hash_table, size_t new_size);
static inline void ensure_valid_bucket_pointers(dshash_table *hash_table);
static inline dshash_table_item *find_in_bucket(dshash_table *hash_table,
												const void *key,
												dsa_pointer item_pointer);
static void insert_item_into_bucket(dshash_table *hash_table,
									dsa_pointer item_pointer,
									dshash_table_item *item,
									dsa_pointer *bucket);
static dshash_table_item *insert_into_bucket(dshash_table *hash_table,
											 const void *key,
											 dsa_pointer *bucket);
static bool delete_key_from_bucket(dshash_table *hash_table,
								   const void *key,
								   dsa_pointer *bucket_head);
static bool delete_item_from_bucket(dshash_table *hash_table,
									dshash_table_item *item,
									dsa_pointer *bucket_head);
static inline dshash_hash hash_key(dshash_table *hash_table, const void *key);
static inline bool equal_keys(dshash_table *hash_table,
							  const void *a, const void *b);

#define PARTITION_LOCK(hash_table, i)			\
	(&(hash_table)->control->partitions[(i)].lock)

#define ASSERT_NO_PARTITION_LOCKS_HELD_BY_ME(hash_table) \
	Assert(!LWLockAnyHeldByMe(&(hash_table)->control->partitions[0].lock, \
		   DSHASH_NUM_PARTITIONS, sizeof(dshash_partition)))

/*
 * Create a new hash table backed by the given dynamic shared area, with the
 * given parameters.  The returned object is allocated in backend-local memory
 * using the current MemoryContext.  'arg' will be passed through to the
 * compare and hash functions.
 */
dshash_table *
dshash_create(dsa_area *area, const dshash_parameters *params, void *arg)
{
	dshash_table *hash_table;
	dsa_pointer control;

	/* Allocate the backend-local object representing the hash table. */
	hash_table = palloc(sizeof(dshash_table));

	/* Allocate the control object in shared memory. */
	control = dsa_allocate(area, sizeof(dshash_table_control));

	/* Set up the local and shared hash table structs. */
	hash_table->area = area;
	hash_table->params = *params;
	hash_table->arg = arg;
	hash_table->control = dsa_get_address(area, control);
	hash_table->control->handle = control;
	hash_table->control->magic = DSHASH_MAGIC;
	hash_table->control->lwlock_tranche_id = params->tranche_id;

	/* Set up the array of lock partitions. */
	{
		dshash_partition *partitions = hash_table->control->partitions;
		int			tranche_id = hash_table->control->lwlock_tranche_id;
		int			i;

		for (i = 0; i < DSHASH_NUM_PARTITIONS; ++i)
		{
			LWLockInitialize(&partitions[i].lock, tranche_id);
			partitions[i].count = 0;
		}
	}

	/*
	 * Set up the initial array of buckets.  Our initial size is the same as
	 * the number of partitions.
	 */
	hash_table->control->size_log2 = DSHASH_NUM_PARTITIONS_LOG2;
	hash_table->control->buckets =
		dsa_allocate_extended(area,
							  sizeof(dsa_pointer) * DSHASH_NUM_PARTITIONS,
							  DSA_ALLOC_NO_OOM | DSA_ALLOC_ZERO);
	if (!DsaPointerIsValid(hash_table->control->buckets))
	{
		dsa_free(area, control);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed on DSA request of size %zu.",
						   sizeof(dsa_pointer) * DSHASH_NUM_PARTITIONS)));
	}
	hash_table->buckets = dsa_get_address(area,
										  hash_table->control->buckets);
	hash_table->size_log2 = hash_table->control->size_log2;

	return hash_table;
}

/*
 * Attach to an existing hash table using a handle.  The returned object is
 * allocated in backend-local memory using the current MemoryContext.  'arg'
 * will be passed through to the compare and hash functions.
 */
dshash_table *
dshash_attach(dsa_area *area, const dshash_parameters *params,
			  dshash_table_handle handle, void *arg)
{
	dshash_table *hash_table;
	dsa_pointer control;

	/* Allocate the backend-local object representing the hash table. */
	hash_table = palloc(sizeof(dshash_table));

	/* Find the control object in shared memory. */
	control = handle;

	/* Set up the local hash table struct. */
	hash_table->area = area;
	hash_table->params = *params;
	hash_table->arg = arg;
	hash_table->control = dsa_get_address(area, control);
	Assert(hash_table->control->magic == DSHASH_MAGIC);

	/*
	 * These will later be set to the correct values by
	 * ensure_valid_bucket_pointers(), at which time we'll be holding a
	 * partition lock for interlocking against concurrent resizing.
	 */
	hash_table->buckets = NULL;
	hash_table->size_log2 = 0;

	return hash_table;
}

/*
 * Detach from a hash table.  This frees backend-local resources associated
 * with the hash table, but the hash table will continue to exist until it is
 * either explicitly destroyed (by a backend that is still attached to it), or
 * the area that backs it is returned to the operating system.
 */
void
dshash_detach(dshash_table *hash_table)
{
	ASSERT_NO_PARTITION_LOCKS_HELD_BY_ME(hash_table);

	/* The hash table may have been destroyed.  Just free local memory. */
	pfree(hash_table);
}

/*
 * Destroy a hash table, returning all memory to the area.  The caller must be
 * certain that no other backend will attempt to access the hash table before
 * calling this function.  Other backend must explicitly call dshash_detach to
 * free up backend-local memory associated with the hash table.  The backend
 * that calls dshash_destroy must not call dshash_detach.
 */
void
dshash_destroy(dshash_table *hash_table)
{
	size_t		size;
	size_t		i;

	Assert(hash_table->control->magic == DSHASH_MAGIC);
	ensure_valid_bucket_pointers(hash_table);

	/* Free all the entries. */
	size = ((size_t) 1) << hash_table->size_log2;
	for (i = 0; i < size; ++i)
	{
		dsa_pointer item_pointer = hash_table->buckets[i];

		while (DsaPointerIsValid(item_pointer))
		{
			dshash_table_item *item;
			dsa_pointer next_item_pointer;

			item = dsa_get_address(hash_table->area, item_pointer);
			next_item_pointer = item->next;
			dsa_free(hash_table->area, item_pointer);
			item_pointer = next_item_pointer;
		}
	}

	/*
	 * Vandalize the control block to help catch programming errors where
	 * other backends access the memory formerly occupied by this hash table.
	 */
	hash_table->control->magic = 0;

	/* Free the active table and control object. */
	dsa_free(hash_table->area, hash_table->control->buckets);
	dsa_free(hash_table->area, hash_table->control->handle);

	pfree(hash_table);
}

/*
 * Get a handle that can be used by other processes to attach to this hash
 * table.
 */
dshash_table_handle
dshash_get_hash_table_handle(dshash_table *hash_table)
{
	Assert(hash_table->control->magic == DSHASH_MAGIC);

	return hash_table->control->handle;
}

/*
 * Look up an entry, given a key.  Returns a pointer to an entry if one can be
 * found with the given key.  Returns NULL if the key is not found.  If a
 * non-NULL value is returned, the entry is locked and must be released by
 * calling dshash_release_lock.  If an error is raised before
 * dshash_release_lock is called, the lock will be released automatically, but
 * the caller must take care to ensure that the entry is not left corrupted.
 * The lock mode is either shared or exclusive depending on 'exclusive'.
 *
 * The caller must not hold a lock already.
 *
 * Note that the lock held is in fact an LWLock, so interrupts will be held on
 * return from this function, and not resumed until dshash_release_lock is
 * called.  It is a very good idea for the caller to release the lock quickly.
 */
void *
dshash_find(dshash_table *hash_table, const void *key, bool exclusive)
{
	dshash_hash hash;
	size_t		partition;
	dshash_table_item *item;

	hash = hash_key(hash_table, key);
	partition = PARTITION_FOR_HASH(hash);

	Assert(hash_table->control->magic == DSHASH_MAGIC);
	ASSERT_NO_PARTITION_LOCKS_HELD_BY_ME(hash_table);

	LWLockAcquire(PARTITION_LOCK(hash_table, partition),
				  exclusive ? LW_EXCLUSIVE : LW_SHARED);
	ensure_valid_bucket_pointers(hash_table);

	/* Search the active bucket. */
	item = find_in_bucket(hash_table, key, BUCKET_FOR_HASH(hash_table, hash));

	if (!item)
	{
		/* Not found. */
		LWLockRelease(PARTITION_LOCK(hash_table, partition));
		return NULL;
	}
	else
	{
		/* The caller will free the lock by calling dshash_release_lock. */
		return ENTRY_FROM_ITEM(item);
	}
}

/*
 * Returns a pointer to an exclusively locked item which must be released with
 * dshash_release_lock.  If the key is found in the hash table, 'found' is set
 * to true and a pointer to the existing entry is returned.  If the key is not
 * found, 'found' is set to false, and a pointer to a newly created entry is
 * returned.
 *
 * Notes above dshash_find() regarding locking and error handling equally
 * apply here.
 */
void *
dshash_find_or_insert(dshash_table *hash_table,
					  const void *key,
					  bool *found)
{
	dshash_hash hash;
	size_t		partition_index;
	dshash_partition *partition;
	dshash_table_item *item;

	hash = hash_key(hash_table, key);
	partition_index = PARTITION_FOR_HASH(hash);
	partition = &hash_table->control->partitions[partition_index];

	Assert(hash_table->control->magic == DSHASH_MAGIC);
	ASSERT_NO_PARTITION_LOCKS_HELD_BY_ME(hash_table);

restart:
	LWLockAcquire(PARTITION_LOCK(hash_table, partition_index),
				  LW_EXCLUSIVE);
	ensure_valid_bucket_pointers(hash_table);

	/* Search the active bucket. */
	item = find_in_bucket(hash_table, key, BUCKET_FOR_HASH(hash_table, hash));

	if (item)
		*found = true;
	else
	{
		*found = false;

		/* Check if we are getting too full. */
		if (partition->count > MAX_COUNT_PER_PARTITION(hash_table))
		{
			/*
			 * The load factor (= keys / buckets) for all buckets protected by
			 * this partition is > 0.75.  Presumably the same applies
			 * generally across the whole hash table (though we don't attempt
			 * to track that directly to avoid contention on some kind of
			 * central counter; we just assume that this partition is
			 * representative).  This is a good time to resize.
			 *
			 * Give up our existing lock first, because resizing needs to
			 * reacquire all the locks in the right order to avoid deadlocks.
			 */
			LWLockRelease(PARTITION_LOCK(hash_table, partition_index));
			resize(hash_table, hash_table->size_log2 + 1);

			goto restart;
		}

		/* Finally we can try to insert the new item. */
		item = insert_into_bucket(hash_table, key,
								  &BUCKET_FOR_HASH(hash_table, hash));
		item->hash = hash;
		/* Adjust per-lock-partition counter for load factor knowledge. */
		++partition->count;
	}

	/* The caller must release the lock with dshash_release_lock. */
	return ENTRY_FROM_ITEM(item);
}

/*
 * Remove an entry by key.  Returns true if the key was found and the
 * corresponding entry was removed.
 *
 * To delete an entry that you already have a pointer to, see
 * dshash_delete_entry.
 */
bool
dshash_delete_key(dshash_table *hash_table, const void *key)
{
	dshash_hash hash;
	size_t		partition;
	bool		found;

	Assert(hash_table->control->magic == DSHASH_MAGIC);
	ASSERT_NO_PARTITION_LOCKS_HELD_BY_ME(hash_table);

	hash = hash_key(hash_table, key);
	partition = PARTITION_FOR_HASH(hash);

	LWLockAcquire(PARTITION_LOCK(hash_table, partition), LW_EXCLUSIVE);
	ensure_valid_bucket_pointers(hash_table);

	if (delete_key_from_bucket(hash_table, key,
							   &BUCKET_FOR_HASH(hash_table, hash)))
	{
		Assert(hash_table->control->partitions[partition].count > 0);
		found = true;
		--hash_table->control->partitions[partition].count;
	}
	else
		found = false;

	LWLockRelease(PARTITION_LOCK(hash_table, partition));

	return found;
}

/*
 * Remove an entry.  The entry must already be exclusively locked, and must
 * have been obtained by dshash_find or dshash_find_or_insert.  Note that this
 * function releases the lock just like dshash_release_lock.
 *
 * To delete an entry by key, see dshash_delete_key.
 */
void
dshash_delete_entry(dshash_table *hash_table, void *entry)
{
	dshash_table_item *item = ITEM_FROM_ENTRY(entry);
	size_t		partition = PARTITION_FOR_HASH(item->hash);

	Assert(hash_table->control->magic == DSHASH_MAGIC);
	Assert(LWLockHeldByMeInMode(PARTITION_LOCK(hash_table, partition),
								LW_EXCLUSIVE));

	delete_item(hash_table, item);
	LWLockRelease(PARTITION_LOCK(hash_table, partition));
}

/*
 * Unlock an entry which was locked by dshash_find or dshash_find_or_insert.
 */
void
dshash_release_lock(dshash_table *hash_table, void *entry)
{
	dshash_table_item *item = ITEM_FROM_ENTRY(entry);
	size_t		partition_index = PARTITION_FOR_HASH(item->hash);

	Assert(hash_table->control->magic == DSHASH_MAGIC);

	LWLockRelease(PARTITION_LOCK(hash_table, partition_index));
}

/*
 * A compare function that forwards to memcmp.
 */
int
dshash_memcmp(const void *a, const void *b, size_t size, void *arg)
{
	return memcmp(a, b, size);
}

/*
 * A hash function that forwards to tag_hash.
 */
dshash_hash
dshash_memhash(const void *v, size_t size, void *arg)
{
	return tag_hash(v, size);
}

/*
 * Print debugging information about the internal state of the hash table to
 * stderr.  The caller must hold no partition locks.
 */
void
dshash_dump(dshash_table *hash_table)
{
	size_t		i;
	size_t		j;

	Assert(hash_table->control->magic == DSHASH_MAGIC);
	ASSERT_NO_PARTITION_LOCKS_HELD_BY_ME(hash_table);

	for (i = 0; i < DSHASH_NUM_PARTITIONS; ++i)
	{
		Assert(!LWLockHeldByMe(PARTITION_LOCK(hash_table, i)));
		LWLockAcquire(PARTITION_LOCK(hash_table, i), LW_SHARED);
	}

	ensure_valid_bucket_pointers(hash_table);

	fprintf(stderr,
			"hash table size = %zu\n", (size_t) 1 << hash_table->size_log2);
	for (i = 0; i < DSHASH_NUM_PARTITIONS; ++i)
	{
		dshash_partition *partition = &hash_table->control->partitions[i];
		size_t		begin = BUCKET_INDEX_FOR_PARTITION(i, hash_table->size_log2);
		size_t		end = BUCKET_INDEX_FOR_PARTITION(i + 1, hash_table->size_log2);

		fprintf(stderr, "  partition %zu\n", i);
		fprintf(stderr,
				"    active buckets (key count = %zu)\n", partition->count);

		for (j = begin; j < end; ++j)
		{
			size_t		count = 0;
			dsa_pointer bucket = hash_table->buckets[j];

			while (DsaPointerIsValid(bucket))
			{
				dshash_table_item *item;

				item = dsa_get_address(hash_table->area, bucket);

				bucket = item->next;
				++count;
			}
			fprintf(stderr, "      bucket %zu (key count = %zu)\n", j, count);
		}
	}

	for (i = 0; i < DSHASH_NUM_PARTITIONS; ++i)
		LWLockRelease(PARTITION_LOCK(hash_table, i));
}

/*
 * Delete a locked item to which we have a pointer.
 */
static void
delete_item(dshash_table *hash_table, dshash_table_item *item)
{
	size_t		hash = item->hash;
	size_t		partition = PARTITION_FOR_HASH(hash);

	Assert(LWLockHeldByMe(PARTITION_LOCK(hash_table, partition)));

	if (delete_item_from_bucket(hash_table, item,
								&BUCKET_FOR_HASH(hash_table, hash)))
	{
		Assert(hash_table->control->partitions[partition].count > 0);
		--hash_table->control->partitions[partition].count;
	}
	else
	{
		Assert(false);
	}
}

/*
 * Grow the hash table if necessary to the requested number of buckets.  The
 * requested size must be double some previously observed size.
 *
 * Must be called without any partition lock held.
 */
static void
resize(dshash_table *hash_table, size_t new_size_log2)
{
	dsa_pointer old_buckets;
	dsa_pointer new_buckets_shared;
	dsa_pointer *new_buckets;
	size_t		size;
	size_t		new_size = ((size_t) 1) << new_size_log2;
	size_t		i;

	/*
	 * Acquire the locks for all lock partitions.  This is expensive, but we
	 * shouldn't have to do it many times.
	 */
	for (i = 0; i < DSHASH_NUM_PARTITIONS; ++i)
	{
		Assert(!LWLockHeldByMe(PARTITION_LOCK(hash_table, i)));

		LWLockAcquire(PARTITION_LOCK(hash_table, i), LW_EXCLUSIVE);
		if (i == 0 && hash_table->control->size_log2 >= new_size_log2)
		{
			/*
			 * Another backend has already increased the size; we can avoid
			 * obtaining all the locks and return early.
			 */
			LWLockRelease(PARTITION_LOCK(hash_table, 0));
			return;
		}
	}

	Assert(new_size_log2 == hash_table->control->size_log2 + 1);

	/* Allocate the space for the new table. */
	new_buckets_shared = dsa_allocate0(hash_table->area,
									   sizeof(dsa_pointer) * new_size);
	new_buckets = dsa_get_address(hash_table->area, new_buckets_shared);

	/*
	 * We've allocated the new bucket array; all that remains to do now is to
	 * reinsert all items, which amounts to adjusting all the pointers.
	 */
	size = ((size_t) 1) << hash_table->control->size_log2;
	for (i = 0; i < size; ++i)
	{
		dsa_pointer item_pointer = hash_table->buckets[i];

		while (DsaPointerIsValid(item_pointer))
		{
			dshash_table_item *item;
			dsa_pointer next_item_pointer;

			item = dsa_get_address(hash_table->area, item_pointer);
			next_item_pointer = item->next;
			insert_item_into_bucket(hash_table, item_pointer, item,
									&new_buckets[BUCKET_INDEX_FOR_HASH_AND_SIZE(item->hash,
																				new_size_log2)]);
			item_pointer = next_item_pointer;
		}
	}

	/* Swap the hash table into place and free the old one. */
	old_buckets = hash_table->control->buckets;
	hash_table->control->buckets = new_buckets_shared;
	hash_table->control->size_log2 = new_size_log2;
	hash_table->buckets = new_buckets;
	dsa_free(hash_table->area, old_buckets);

	/* Release all the locks. */
	for (i = 0; i < DSHASH_NUM_PARTITIONS; ++i)
		LWLockRelease(PARTITION_LOCK(hash_table, i));
}

/*
 * Make sure that our backend-local bucket pointers are up to date.  The
 * caller must have locked one lock partition, which prevents resize() from
 * running concurrently.
 */
static inline void
ensure_valid_bucket_pointers(dshash_table *hash_table)
{
	if (hash_table->size_log2 != hash_table->control->size_log2)
	{
		hash_table->buckets = dsa_get_address(hash_table->area,
											  hash_table->control->buckets);
		hash_table->size_log2 = hash_table->control->size_log2;
	}
}

/*
 * Scan a locked bucket for a match, using the provided compare function.
 */
static inline dshash_table_item *
find_in_bucket(dshash_table *hash_table, const void *key,
			   dsa_pointer item_pointer)
{
	while (DsaPointerIsValid(item_pointer))
	{
		dshash_table_item *item;

		item = dsa_get_address(hash_table->area, item_pointer);
		if (equal_keys(hash_table, key, ENTRY_FROM_ITEM(item)))
			return item;
		item_pointer = item->next;
	}
	return NULL;
}

/*
 * Insert an already-allocated item into a bucket.
 */
static void
insert_item_into_bucket(dshash_table *hash_table,
						dsa_pointer item_pointer,
						dshash_table_item *item,
						dsa_pointer *bucket)
{
	Assert(item == dsa_get_address(hash_table->area, item_pointer));

	item->next = *bucket;
	*bucket = item_pointer;
}

/*
 * Allocate space for an entry with the given key and insert it into the
 * provided bucket.
 */
static dshash_table_item *
insert_into_bucket(dshash_table *hash_table,
				   const void *key,
				   dsa_pointer *bucket)
{
	dsa_pointer item_pointer;
	dshash_table_item *item;

	item_pointer = dsa_allocate(hash_table->area,
								hash_table->params.entry_size +
								MAXALIGN(sizeof(dshash_table_item)));
	item = dsa_get_address(hash_table->area, item_pointer);
	memcpy(ENTRY_FROM_ITEM(item), key, hash_table->params.key_size);
	insert_item_into_bucket(hash_table, item_pointer, item, bucket);
	return item;
}

/*
 * Search a bucket for a matching key and delete it.
 */
static bool
delete_key_from_bucket(dshash_table *hash_table,
					   const void *key,
					   dsa_pointer *bucket_head)
{
	while (DsaPointerIsValid(*bucket_head))
	{
		dshash_table_item *item;

		item = dsa_get_address(hash_table->area, *bucket_head);

		if (equal_keys(hash_table, key, ENTRY_FROM_ITEM(item)))
		{
			dsa_pointer next;

			next = item->next;
			dsa_free(hash_table->area, *bucket_head);
			*bucket_head = next;

			return true;
		}
		bucket_head = &item->next;
	}
	return false;
}

/*
 * Delete the specified item from the bucket.
 */
static bool
delete_item_from_bucket(dshash_table *hash_table,
						dshash_table_item *item,
						dsa_pointer *bucket_head)
{
	while (DsaPointerIsValid(*bucket_head))
	{
		dshash_table_item *bucket_item;

		bucket_item = dsa_get_address(hash_table->area, *bucket_head);

		if (bucket_item == item)
		{
			dsa_pointer next;

			next = item->next;
			dsa_free(hash_table->area, *bucket_head);
			*bucket_head = next;
			return true;
		}
		bucket_head = &bucket_item->next;
	}
	return false;
}

/*
 * Compute the hash value for a key.
 */
static inline dshash_hash
hash_key(dshash_table *hash_table, const void *key)
{
	return hash_table->params.hash_function(key,
											hash_table->params.key_size,
											hash_table->arg);
}

/*
 * Check whether two keys compare equal.
 */
static inline bool
equal_keys(dshash_table *hash_table, const void *a, const void *b)
{
	return hash_table->params.compare_function(a, b,
											   hash_table->params.key_size,
											   hash_table->arg) == 0;
}
