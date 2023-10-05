/*-------------------------------------------------------------------------
 *
 * nodeMemoize.c
 *	  Routines to handle caching of results from parameterized nodes
 *
 * Portions Copyright (c) 2021-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeMemoize.c
 *
 * Memoize nodes are intended to sit above parameterized nodes in the plan
 * tree in order to cache results from them.  The intention here is that a
 * repeat scan with a parameter value that has already been seen by the node
 * can fetch tuples from the cache rather than having to re-scan the outer
 * node all over again.  The query planner may choose to make use of one of
 * these when it thinks rescans for previously seen values are likely enough
 * to warrant adding the additional node.
 *
 * The method of cache we use is a hash table.  When the cache fills, we never
 * spill tuples to disk, instead, we choose to evict the least recently used
 * cache entry from the cache.  We remember the least recently used entry by
 * always pushing new entries and entries we look for onto the tail of a
 * doubly linked list.  This means that older items always bubble to the top
 * of this LRU list.
 *
 * Sometimes our callers won't run their scans to completion. For example a
 * semi-join only needs to run until it finds a matching tuple, and once it
 * does, the join operator skips to the next outer tuple and does not execute
 * the inner side again on that scan.  Because of this, we must keep track of
 * when a cache entry is complete, and by default, we know it is when we run
 * out of tuples to read during the scan.  However, there are cases where we
 * can mark the cache entry as complete without exhausting the scan of all
 * tuples.  One case is unique joins, where the join operator knows that there
 * will only be at most one match for any given outer tuple.  In order to
 * support such cases we allow the "singlerow" option to be set for the cache.
 * This option marks the cache entry as complete after we read the first tuple
 * from the subnode.
 *
 * It's possible when we're filling the cache for a given set of parameters
 * that we're unable to free enough memory to store any more tuples.  If this
 * happens then we'll have already evicted all other cache entries.  When
 * caching another tuple would cause us to exceed our memory budget, we must
 * free the entry that we're currently populating and move the state machine
 * into MEMO_CACHE_BYPASS_MODE.  This means that we'll not attempt to cache
 * any further tuples for this particular scan.  We don't have the memory for
 * it.  The state machine will be reset again on the next rescan.  If the
 * memory requirements to cache the next parameter's tuples are less
 * demanding, then that may allow us to start putting useful entries back into
 * the cache again.
 *
 *
 * INTERFACE ROUTINES
 *		ExecMemoize			- lookup cache, exec subplan when not found
 *		ExecInitMemoize		- initialize node and subnodes
 *		ExecEndMemoize		- shutdown node and subnodes
 *		ExecReScanMemoize	- rescan the memoize node
 *
 *		ExecMemoizeEstimate		estimates DSM space needed for parallel plan
 *		ExecMemoizeInitializeDSM initialize DSM for parallel plan
 *		ExecMemoizeInitializeWorker attach to DSM info in parallel worker
 *		ExecMemoizeRetrieveInstrumentation get instrumentation from worker
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/nodeMemoize.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"

/* States of the ExecMemoize state machine */
#define MEMO_CACHE_LOOKUP			1	/* Attempt to perform a cache lookup */
#define MEMO_CACHE_FETCH_NEXT_TUPLE	2	/* Get another tuple from the cache */
#define MEMO_FILLING_CACHE			3	/* Read outer node to fill cache */
#define MEMO_CACHE_BYPASS_MODE		4	/* Bypass mode.  Just read from our
										 * subplan without caching anything */
#define MEMO_END_OF_SCAN			5	/* Ready for rescan */


/* Helper macros for memory accounting */
#define EMPTY_ENTRY_MEMORY_BYTES(e)		(sizeof(MemoizeEntry) + \
										 sizeof(MemoizeKey) + \
										 (e)->key->params->t_len);
#define CACHE_TUPLE_BYTES(t)			(sizeof(MemoizeTuple) + \
										 (t)->mintuple->t_len)

 /* MemoizeTuple Stores an individually cached tuple */
typedef struct MemoizeTuple
{
	MinimalTuple mintuple;		/* Cached tuple */
	struct MemoizeTuple *next;	/* The next tuple with the same parameter
								 * values or NULL if it's the last one */
} MemoizeTuple;

/*
 * MemoizeKey
 * The hash table key for cached entries plus the LRU list link
 */
typedef struct MemoizeKey
{
	MinimalTuple params;
	dlist_node	lru_node;		/* Pointer to next/prev key in LRU list */
} MemoizeKey;

/*
 * MemoizeEntry
 *		The data struct that the cache hash table stores
 */
typedef struct MemoizeEntry
{
	MemoizeKey *key;			/* Hash key for hash table lookups */
	MemoizeTuple *tuplehead;	/* Pointer to the first tuple or NULL if no
								 * tuples are cached for this entry */
	uint32		hash;			/* Hash value (cached) */
	char		status;			/* Hash status */
	bool		complete;		/* Did we read the outer plan to completion? */
} MemoizeEntry;


#define SH_PREFIX memoize
#define SH_ELEMENT_TYPE MemoizeEntry
#define SH_KEY_TYPE MemoizeKey *
#define SH_SCOPE static inline
#define SH_DECLARE
#include "lib/simplehash.h"

static uint32 MemoizeHash_hash(struct memoize_hash *tb,
							   const MemoizeKey *key);
static bool MemoizeHash_equal(struct memoize_hash *tb,
							  const MemoizeKey *params1,
							  const MemoizeKey *params2);

#define SH_PREFIX memoize
#define SH_ELEMENT_TYPE MemoizeEntry
#define SH_KEY_TYPE MemoizeKey *
#define SH_KEY key
#define SH_HASH_KEY(tb, key) MemoizeHash_hash(tb, key)
#define SH_EQUAL(tb, a, b) MemoizeHash_equal(tb, a, b)
#define SH_SCOPE static inline
#define SH_STORE_HASH
#define SH_GET_HASH(tb, a) a->hash
#define SH_DEFINE
#include "lib/simplehash.h"

/*
 * MemoizeHash_hash
 *		Hash function for simplehash hashtable.  'key' is unused here as we
 *		require that all table lookups first populate the MemoizeState's
 *		probeslot with the key values to be looked up.
 */
static uint32
MemoizeHash_hash(struct memoize_hash *tb, const MemoizeKey *key)
{
	MemoizeState *mstate = (MemoizeState *) tb->private_data;
	ExprContext *econtext = mstate->ss.ps.ps_ExprContext;
	MemoryContext oldcontext;
	TupleTableSlot *pslot = mstate->probeslot;
	uint32		hashkey = 0;
	int			numkeys = mstate->nkeys;

	oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	if (mstate->binary_mode)
	{
		for (int i = 0; i < numkeys; i++)
		{
			/* combine successive hashkeys by rotating */
			hashkey = pg_rotate_left32(hashkey, 1);

			if (!pslot->tts_isnull[i])	/* treat nulls as having hash key 0 */
			{
				FormData_pg_attribute *attr;
				uint32		hkey;

				attr = &pslot->tts_tupleDescriptor->attrs[i];

				hkey = datum_image_hash(pslot->tts_values[i], attr->attbyval, attr->attlen);

				hashkey ^= hkey;
			}
		}
	}
	else
	{
		FmgrInfo   *hashfunctions = mstate->hashfunctions;
		Oid		   *collations = mstate->collations;

		for (int i = 0; i < numkeys; i++)
		{
			/* combine successive hashkeys by rotating */
			hashkey = pg_rotate_left32(hashkey, 1);

			if (!pslot->tts_isnull[i])	/* treat nulls as having hash key 0 */
			{
				uint32		hkey;

				hkey = DatumGetUInt32(FunctionCall1Coll(&hashfunctions[i],
														collations[i], pslot->tts_values[i]));
				hashkey ^= hkey;
			}
		}
	}

	ResetExprContext(econtext);
	MemoryContextSwitchTo(oldcontext);
	return murmurhash32(hashkey);
}

/*
 * MemoizeHash_equal
 *		Equality function for confirming hash value matches during a hash
 *		table lookup.  'key2' is never used.  Instead the MemoizeState's
 *		probeslot is always populated with details of what's being looked up.
 */
static bool
MemoizeHash_equal(struct memoize_hash *tb, const MemoizeKey *key1,
				  const MemoizeKey *key2)
{
	MemoizeState *mstate = (MemoizeState *) tb->private_data;
	ExprContext *econtext = mstate->ss.ps.ps_ExprContext;
	TupleTableSlot *tslot = mstate->tableslot;
	TupleTableSlot *pslot = mstate->probeslot;

	/* probeslot should have already been prepared by prepare_probe_slot() */
	ExecStoreMinimalTuple(key1->params, tslot, false);

	if (mstate->binary_mode)
	{
		MemoryContext oldcontext;
		int			numkeys = mstate->nkeys;
		bool		match = true;

		oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

		slot_getallattrs(tslot);
		slot_getallattrs(pslot);

		for (int i = 0; i < numkeys; i++)
		{
			FormData_pg_attribute *attr;

			if (tslot->tts_isnull[i] != pslot->tts_isnull[i])
			{
				match = false;
				break;
			}

			/* both NULL? they're equal */
			if (tslot->tts_isnull[i])
				continue;

			/* perform binary comparison on the two datums */
			attr = &tslot->tts_tupleDescriptor->attrs[i];
			if (!datum_image_eq(tslot->tts_values[i], pslot->tts_values[i],
								attr->attbyval, attr->attlen))
			{
				match = false;
				break;
			}
		}

		ResetExprContext(econtext);
		MemoryContextSwitchTo(oldcontext);
		return match;
	}
	else
	{
		econtext->ecxt_innertuple = tslot;
		econtext->ecxt_outertuple = pslot;
		return ExecQualAndReset(mstate->cache_eq_expr, econtext);
	}
}

/*
 * Initialize the hash table to empty.
 */
static void
build_hash_table(MemoizeState *mstate, uint32 size)
{
	/* Make a guess at a good size when we're not given a valid size. */
	if (size == 0)
		size = 1024;

	/* memoize_create will convert the size to a power of 2 */
	mstate->hashtable = memoize_create(mstate->tableContext, size, mstate);
}

/*
 * prepare_probe_slot
 *		Populate mstate's probeslot with the values from the tuple stored
 *		in 'key'.  If 'key' is NULL, then perform the population by evaluating
 *		mstate's param_exprs.
 */
static inline void
prepare_probe_slot(MemoizeState *mstate, MemoizeKey *key)
{
	TupleTableSlot *pslot = mstate->probeslot;
	TupleTableSlot *tslot = mstate->tableslot;
	int			numKeys = mstate->nkeys;

	ExecClearTuple(pslot);

	if (key == NULL)
	{
		ExprContext *econtext = mstate->ss.ps.ps_ExprContext;
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

		/* Set the probeslot's values based on the current parameter values */
		for (int i = 0; i < numKeys; i++)
			pslot->tts_values[i] = ExecEvalExpr(mstate->param_exprs[i],
												econtext,
												&pslot->tts_isnull[i]);

		MemoryContextSwitchTo(oldcontext);
	}
	else
	{
		/* Process the key's MinimalTuple and store the values in probeslot */
		ExecStoreMinimalTuple(key->params, tslot, false);
		slot_getallattrs(tslot);
		memcpy(pslot->tts_values, tslot->tts_values, sizeof(Datum) * numKeys);
		memcpy(pslot->tts_isnull, tslot->tts_isnull, sizeof(bool) * numKeys);
	}

	ExecStoreVirtualTuple(pslot);
}

/*
 * entry_purge_tuples
 *		Remove all tuples from the cache entry pointed to by 'entry'.  This
 *		leaves an empty cache entry.  Also, update the memory accounting to
 *		reflect the removal of the tuples.
 */
static inline void
entry_purge_tuples(MemoizeState *mstate, MemoizeEntry *entry)
{
	MemoizeTuple *tuple = entry->tuplehead;
	uint64		freed_mem = 0;

	while (tuple != NULL)
	{
		MemoizeTuple *next = tuple->next;

		freed_mem += CACHE_TUPLE_BYTES(tuple);

		/* Free memory used for this tuple */
		pfree(tuple->mintuple);
		pfree(tuple);

		tuple = next;
	}

	entry->complete = false;
	entry->tuplehead = NULL;

	/* Update the memory accounting */
	mstate->mem_used -= freed_mem;
}

/*
 * remove_cache_entry
 *		Remove 'entry' from the cache and free memory used by it.
 */
static void
remove_cache_entry(MemoizeState *mstate, MemoizeEntry *entry)
{
	MemoizeKey *key = entry->key;

	dlist_delete(&entry->key->lru_node);

	/* Remove all of the tuples from this entry */
	entry_purge_tuples(mstate, entry);

	/*
	 * Update memory accounting. entry_purge_tuples should have already
	 * subtracted the memory used for each cached tuple.  Here we just update
	 * the amount used by the entry itself.
	 */
	mstate->mem_used -= EMPTY_ENTRY_MEMORY_BYTES(entry);

	/* Remove the entry from the cache */
	memoize_delete_item(mstate->hashtable, entry);

	pfree(key->params);
	pfree(key);
}

/*
 * cache_purge_all
 *		Remove all items from the cache
 */
static void
cache_purge_all(MemoizeState *mstate)
{
	uint64		evictions = mstate->hashtable->members;
	PlanState  *pstate = (PlanState *) mstate;

	/*
	 * Likely the most efficient way to remove all items is to just reset the
	 * memory context for the cache and then rebuild a fresh hash table.  This
	 * saves having to remove each item one by one and pfree each cached tuple
	 */
	MemoryContextReset(mstate->tableContext);

	/* Make the hash table the same size as the original size */
	build_hash_table(mstate, ((Memoize *) pstate->plan)->est_entries);

	/* reset the LRU list */
	dlist_init(&mstate->lru_list);
	mstate->last_tuple = NULL;
	mstate->entry = NULL;

	mstate->mem_used = 0;

	/* XXX should we add something new to track these purges? */
	mstate->stats.cache_evictions += evictions; /* Update Stats */
}

/*
 * cache_reduce_memory
 *		Evict older and less recently used items from the cache in order to
 *		reduce the memory consumption back to something below the
 *		MemoizeState's mem_limit.
 *
 * 'specialkey', if not NULL, causes the function to return false if the entry
 * which the key belongs to is removed from the cache.
 */
static bool
cache_reduce_memory(MemoizeState *mstate, MemoizeKey *specialkey)
{
	bool		specialkey_intact = true;	/* for now */
	dlist_mutable_iter iter;
	uint64		evictions = 0;

	/* Update peak memory usage */
	if (mstate->mem_used > mstate->stats.mem_peak)
		mstate->stats.mem_peak = mstate->mem_used;

	/* We expect only to be called when we've gone over budget on memory */
	Assert(mstate->mem_used > mstate->mem_limit);

	/* Start the eviction process starting at the head of the LRU list. */
	dlist_foreach_modify(iter, &mstate->lru_list)
	{
		MemoizeKey *key = dlist_container(MemoizeKey, lru_node, iter.cur);
		MemoizeEntry *entry;

		/*
		 * Populate the hash probe slot in preparation for looking up this LRU
		 * entry.
		 */
		prepare_probe_slot(mstate, key);

		/*
		 * Ideally the LRU list pointers would be stored in the entry itself
		 * rather than in the key.  Unfortunately, we can't do that as the
		 * simplehash.h code may resize the table and allocate new memory for
		 * entries which would result in those pointers pointing to the old
		 * buckets.  However, it's fine to use the key to store this as that's
		 * only referenced by a pointer in the entry, which of course follows
		 * the entry whenever the hash table is resized.  Since we only have a
		 * pointer to the key here, we must perform a hash table lookup to
		 * find the entry that the key belongs to.
		 */
		entry = memoize_lookup(mstate->hashtable, NULL);

		/*
		 * Sanity check that we found the entry belonging to the LRU list
		 * item.  A misbehaving hash or equality function could cause the
		 * entry not to be found or the wrong entry to be found.
		 */
		if (unlikely(entry == NULL || entry->key != key))
			elog(ERROR, "could not find memoization table entry");

		/*
		 * If we're being called to free memory while the cache is being
		 * populated with new tuples, then we'd better take some care as we
		 * could end up freeing the entry which 'specialkey' belongs to.
		 * Generally callers will pass 'specialkey' as the key for the cache
		 * entry which is currently being populated, so we must set
		 * 'specialkey_intact' to false to inform the caller the specialkey
		 * entry has been removed.
		 */
		if (key == specialkey)
			specialkey_intact = false;

		/*
		 * Finally remove the entry.  This will remove from the LRU list too.
		 */
		remove_cache_entry(mstate, entry);

		evictions++;

		/* Exit if we've freed enough memory */
		if (mstate->mem_used <= mstate->mem_limit)
			break;
	}

	mstate->stats.cache_evictions += evictions; /* Update Stats */

	return specialkey_intact;
}

/*
 * cache_lookup
 *		Perform a lookup to see if we've already cached tuples based on the
 *		scan's current parameters.  If we find an existing entry we move it to
 *		the end of the LRU list, set *found to true then return it.  If we
 *		don't find an entry then we create a new one and add it to the end of
 *		the LRU list.  We also update cache memory accounting and remove older
 *		entries if we go over the memory budget.  If we managed to free enough
 *		memory we return the new entry, else we return NULL.
 *
 * Callers can assume we'll never return NULL when *found is true.
 */
static MemoizeEntry *
cache_lookup(MemoizeState *mstate, bool *found)
{
	MemoizeKey *key;
	MemoizeEntry *entry;
	MemoryContext oldcontext;

	/* prepare the probe slot with the current scan parameters */
	prepare_probe_slot(mstate, NULL);

	/*
	 * Add the new entry to the cache.  No need to pass a valid key since the
	 * hash function uses mstate's probeslot, which we populated above.
	 */
	entry = memoize_insert(mstate->hashtable, NULL, found);

	if (*found)
	{
		/*
		 * Move existing entry to the tail of the LRU list to mark it as the
		 * most recently used item.
		 */
		dlist_move_tail(&mstate->lru_list, &entry->key->lru_node);

		return entry;
	}

	oldcontext = MemoryContextSwitchTo(mstate->tableContext);

	/* Allocate a new key */
	entry->key = key = (MemoizeKey *) palloc(sizeof(MemoizeKey));
	key->params = ExecCopySlotMinimalTuple(mstate->probeslot);

	/* Update the total cache memory utilization */
	mstate->mem_used += EMPTY_ENTRY_MEMORY_BYTES(entry);

	/* Initialize this entry */
	entry->complete = false;
	entry->tuplehead = NULL;

	/*
	 * Since this is the most recently used entry, push this entry onto the
	 * end of the LRU list.
	 */
	dlist_push_tail(&mstate->lru_list, &entry->key->lru_node);

	mstate->last_tuple = NULL;

	MemoryContextSwitchTo(oldcontext);

	/*
	 * If we've gone over our memory budget, then we'll free up some space in
	 * the cache.
	 */
	if (mstate->mem_used > mstate->mem_limit)
	{
		/*
		 * Try to free up some memory.  It's highly unlikely that we'll fail
		 * to do so here since the entry we've just added is yet to contain
		 * any tuples and we're able to remove any other entry to reduce the
		 * memory consumption.
		 */
		if (unlikely(!cache_reduce_memory(mstate, key)))
			return NULL;

		/*
		 * The process of removing entries from the cache may have caused the
		 * code in simplehash.h to shuffle elements to earlier buckets in the
		 * hash table.  If it has, we'll need to find the entry again by
		 * performing a lookup.  Fortunately, we can detect if this has
		 * happened by seeing if the entry is still in use and that the key
		 * pointer matches our expected key.
		 */
		if (entry->status != memoize_SH_IN_USE || entry->key != key)
		{
			/*
			 * We need to repopulate the probeslot as lookups performed during
			 * the cache evictions above will have stored some other key.
			 */
			prepare_probe_slot(mstate, key);

			/* Re-find the newly added entry */
			entry = memoize_lookup(mstate->hashtable, NULL);
			Assert(entry != NULL);
		}
	}

	return entry;
}

/*
 * cache_store_tuple
 *		Add the tuple stored in 'slot' to the mstate's current cache entry.
 *		The cache entry must have already been made with cache_lookup().
 *		mstate's last_tuple field must point to the tail of mstate->entry's
 *		list of tuples.
 */
static bool
cache_store_tuple(MemoizeState *mstate, TupleTableSlot *slot)
{
	MemoizeTuple *tuple;
	MemoizeEntry *entry = mstate->entry;
	MemoryContext oldcontext;

	Assert(slot != NULL);
	Assert(entry != NULL);

	oldcontext = MemoryContextSwitchTo(mstate->tableContext);

	tuple = (MemoizeTuple *) palloc(sizeof(MemoizeTuple));
	tuple->mintuple = ExecCopySlotMinimalTuple(slot);
	tuple->next = NULL;

	/* Account for the memory we just consumed */
	mstate->mem_used += CACHE_TUPLE_BYTES(tuple);

	if (entry->tuplehead == NULL)
	{
		/*
		 * This is the first tuple for this entry, so just point the list head
		 * to it.
		 */
		entry->tuplehead = tuple;
	}
	else
	{
		/* push this tuple onto the tail of the list */
		mstate->last_tuple->next = tuple;
	}

	mstate->last_tuple = tuple;
	MemoryContextSwitchTo(oldcontext);

	/*
	 * If we've gone over our memory budget then free up some space in the
	 * cache.
	 */
	if (mstate->mem_used > mstate->mem_limit)
	{
		MemoizeKey *key = entry->key;

		if (!cache_reduce_memory(mstate, key))
			return false;

		/*
		 * The process of removing entries from the cache may have caused the
		 * code in simplehash.h to shuffle elements to earlier buckets in the
		 * hash table.  If it has, we'll need to find the entry again by
		 * performing a lookup.  Fortunately, we can detect if this has
		 * happened by seeing if the entry is still in use and that the key
		 * pointer matches our expected key.
		 */
		if (entry->status != memoize_SH_IN_USE || entry->key != key)
		{
			/*
			 * We need to repopulate the probeslot as lookups performed during
			 * the cache evictions above will have stored some other key.
			 */
			prepare_probe_slot(mstate, key);

			/* Re-find the entry */
			mstate->entry = entry = memoize_lookup(mstate->hashtable, NULL);
			Assert(entry != NULL);
		}
	}

	return true;
}

static TupleTableSlot *
ExecMemoize(PlanState *pstate)
{
	MemoizeState *node = castNode(MemoizeState, pstate);
	PlanState  *outerNode;
	TupleTableSlot *slot;

	switch (node->mstatus)
	{
		case MEMO_CACHE_LOOKUP:
			{
				MemoizeEntry *entry;
				TupleTableSlot *outerslot;
				bool		found;

				Assert(node->entry == NULL);

				/*
				 * We're only ever in this state for the first call of the
				 * scan.  Here we have a look to see if we've already seen the
				 * current parameters before and if we have already cached a
				 * complete set of records that the outer plan will return for
				 * these parameters.
				 *
				 * When we find a valid cache entry, we'll return the first
				 * tuple from it. If not found, we'll create a cache entry and
				 * then try to fetch a tuple from the outer scan.  If we find
				 * one there, we'll try to cache it.
				 */

				/* see if we've got anything cached for the current parameters */
				entry = cache_lookup(node, &found);

				if (found && entry->complete)
				{
					node->stats.cache_hits += 1;	/* stats update */

					/*
					 * Set last_tuple and entry so that the state
					 * MEMO_CACHE_FETCH_NEXT_TUPLE can easily find the next
					 * tuple for these parameters.
					 */
					node->last_tuple = entry->tuplehead;
					node->entry = entry;

					/* Fetch the first cached tuple, if there is one */
					if (entry->tuplehead)
					{
						node->mstatus = MEMO_CACHE_FETCH_NEXT_TUPLE;

						slot = node->ss.ps.ps_ResultTupleSlot;
						ExecStoreMinimalTuple(entry->tuplehead->mintuple,
											  slot, false);

						return slot;
					}

					/* The cache entry is void of any tuples. */
					node->mstatus = MEMO_END_OF_SCAN;
					return NULL;
				}

				/* Handle cache miss */
				node->stats.cache_misses += 1;	/* stats update */

				if (found)
				{
					/*
					 * A cache entry was found, but the scan for that entry
					 * did not run to completion.  We'll just remove all
					 * tuples and start again.  It might be tempting to
					 * continue where we left off, but there's no guarantee
					 * the outer node will produce the tuples in the same
					 * order as it did last time.
					 */
					entry_purge_tuples(node, entry);
				}

				/* Scan the outer node for a tuple to cache */
				outerNode = outerPlanState(node);
				outerslot = ExecProcNode(outerNode);
				if (TupIsNull(outerslot))
				{
					/*
					 * cache_lookup may have returned NULL due to failure to
					 * free enough cache space, so ensure we don't do anything
					 * here that assumes it worked. There's no need to go into
					 * bypass mode here as we're setting mstatus to end of
					 * scan.
					 */
					if (likely(entry))
						entry->complete = true;

					node->mstatus = MEMO_END_OF_SCAN;
					return NULL;
				}

				node->entry = entry;

				/*
				 * If we failed to create the entry or failed to store the
				 * tuple in the entry, then go into bypass mode.
				 */
				if (unlikely(entry == NULL ||
							 !cache_store_tuple(node, outerslot)))
				{
					node->stats.cache_overflows += 1;	/* stats update */

					node->mstatus = MEMO_CACHE_BYPASS_MODE;

					/*
					 * No need to clear out last_tuple as we'll stay in bypass
					 * mode until the end of the scan.
					 */
				}
				else
				{
					/*
					 * If we only expect a single row from this scan then we
					 * can mark that we're not expecting more.  This allows
					 * cache lookups to work even when the scan has not been
					 * executed to completion.
					 */
					entry->complete = node->singlerow;
					node->mstatus = MEMO_FILLING_CACHE;
				}

				slot = node->ss.ps.ps_ResultTupleSlot;
				ExecCopySlot(slot, outerslot);
				return slot;
			}

		case MEMO_CACHE_FETCH_NEXT_TUPLE:
			{
				/* We shouldn't be in this state if these are not set */
				Assert(node->entry != NULL);
				Assert(node->last_tuple != NULL);

				/* Skip to the next tuple to output */
				node->last_tuple = node->last_tuple->next;

				/* No more tuples in the cache */
				if (node->last_tuple == NULL)
				{
					node->mstatus = MEMO_END_OF_SCAN;
					return NULL;
				}

				slot = node->ss.ps.ps_ResultTupleSlot;
				ExecStoreMinimalTuple(node->last_tuple->mintuple, slot,
									  false);

				return slot;
			}

		case MEMO_FILLING_CACHE:
			{
				TupleTableSlot *outerslot;
				MemoizeEntry *entry = node->entry;

				/* entry should already have been set by MEMO_CACHE_LOOKUP */
				Assert(entry != NULL);

				/*
				 * When in the MEMO_FILLING_CACHE state, we've just had a
				 * cache miss and are populating the cache with the current
				 * scan tuples.
				 */
				outerNode = outerPlanState(node);
				outerslot = ExecProcNode(outerNode);
				if (TupIsNull(outerslot))
				{
					/* No more tuples.  Mark it as complete */
					entry->complete = true;
					node->mstatus = MEMO_END_OF_SCAN;
					return NULL;
				}

				/*
				 * Validate if the planner properly set the singlerow flag. It
				 * should only set that if each cache entry can, at most,
				 * return 1 row.
				 */
				if (unlikely(entry->complete))
					elog(ERROR, "cache entry already complete");

				/* Record the tuple in the current cache entry */
				if (unlikely(!cache_store_tuple(node, outerslot)))
				{
					/* Couldn't store it?  Handle overflow */
					node->stats.cache_overflows += 1;	/* stats update */

					node->mstatus = MEMO_CACHE_BYPASS_MODE;

					/*
					 * No need to clear out entry or last_tuple as we'll stay
					 * in bypass mode until the end of the scan.
					 */
				}

				slot = node->ss.ps.ps_ResultTupleSlot;
				ExecCopySlot(slot, outerslot);
				return slot;
			}

		case MEMO_CACHE_BYPASS_MODE:
			{
				TupleTableSlot *outerslot;

				/*
				 * When in bypass mode we just continue to read tuples without
				 * caching.  We need to wait until the next rescan before we
				 * can come out of this mode.
				 */
				outerNode = outerPlanState(node);
				outerslot = ExecProcNode(outerNode);
				if (TupIsNull(outerslot))
				{
					node->mstatus = MEMO_END_OF_SCAN;
					return NULL;
				}

				slot = node->ss.ps.ps_ResultTupleSlot;
				ExecCopySlot(slot, outerslot);
				return slot;
			}

		case MEMO_END_OF_SCAN:

			/*
			 * We've already returned NULL for this scan, but just in case
			 * something calls us again by mistake.
			 */
			return NULL;

		default:
			elog(ERROR, "unrecognized memoize state: %d",
				 (int) node->mstatus);
			return NULL;
	}							/* switch */
}

MemoizeState *
ExecInitMemoize(Memoize *node, EState *estate, int eflags)
{
	MemoizeState *mstate = makeNode(MemoizeState);
	Plan	   *outerNode;
	int			i;
	int			nkeys;
	Oid		   *eqfuncoids;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	mstate->ss.ps.plan = (Plan *) node;
	mstate->ss.ps.state = estate;
	mstate->ss.ps.ExecProcNode = ExecMemoize;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &mstate->ss.ps);

	outerNode = outerPlan(node);
	outerPlanState(mstate) = ExecInitNode(outerNode, estate, eflags);

	/*
	 * Initialize return slot and type. No need to initialize projection info
	 * because this node doesn't do projections.
	 */
	ExecInitResultTupleSlotTL(&mstate->ss.ps, &TTSOpsMinimalTuple);
	mstate->ss.ps.ps_ProjInfo = NULL;

	/*
	 * Initialize scan slot and type.
	 */
	ExecCreateScanSlotFromOuterPlan(estate, &mstate->ss, &TTSOpsMinimalTuple);

	/*
	 * Set the state machine to lookup the cache.  We won't find anything
	 * until we cache something, but this saves a special case to create the
	 * first entry.
	 */
	mstate->mstatus = MEMO_CACHE_LOOKUP;

	mstate->nkeys = nkeys = node->numKeys;
	mstate->hashkeydesc = ExecTypeFromExprList(node->param_exprs);
	mstate->tableslot = MakeSingleTupleTableSlot(mstate->hashkeydesc,
												 &TTSOpsMinimalTuple);
	mstate->probeslot = MakeSingleTupleTableSlot(mstate->hashkeydesc,
												 &TTSOpsVirtual);

	mstate->param_exprs = (ExprState **) palloc(nkeys * sizeof(ExprState *));
	mstate->collations = node->collations;	/* Just point directly to the plan
											 * data */
	mstate->hashfunctions = (FmgrInfo *) palloc(nkeys * sizeof(FmgrInfo));

	eqfuncoids = palloc(nkeys * sizeof(Oid));

	for (i = 0; i < nkeys; i++)
	{
		Oid			hashop = node->hashOperators[i];
		Oid			left_hashfn;
		Oid			right_hashfn;
		Expr	   *param_expr = (Expr *) list_nth(node->param_exprs, i);

		if (!get_op_hash_functions(hashop, &left_hashfn, &right_hashfn))
			elog(ERROR, "could not find hash function for hash operator %u",
				 hashop);

		fmgr_info(left_hashfn, &mstate->hashfunctions[i]);

		mstate->param_exprs[i] = ExecInitExpr(param_expr, (PlanState *) mstate);
		eqfuncoids[i] = get_opcode(hashop);
	}

	mstate->cache_eq_expr = ExecBuildParamSetEqual(mstate->hashkeydesc,
												   &TTSOpsMinimalTuple,
												   &TTSOpsVirtual,
												   eqfuncoids,
												   node->collations,
												   node->param_exprs,
												   (PlanState *) mstate);

	pfree(eqfuncoids);
	mstate->mem_used = 0;

	/* Limit the total memory consumed by the cache to this */
	mstate->mem_limit = get_hash_memory_limit();

	/* A memory context dedicated for the cache */
	mstate->tableContext = AllocSetContextCreate(CurrentMemoryContext,
												 "MemoizeHashTable",
												 ALLOCSET_DEFAULT_SIZES);

	dlist_init(&mstate->lru_list);
	mstate->last_tuple = NULL;
	mstate->entry = NULL;

	/*
	 * Mark if we can assume the cache entry is completed after we get the
	 * first record for it.  Some callers might not call us again after
	 * getting the first match. e.g. A join operator performing a unique join
	 * is able to skip to the next outer tuple after getting the first
	 * matching inner tuple.  In this case, the cache entry is complete after
	 * getting the first tuple.  This allows us to mark it as so.
	 */
	mstate->singlerow = node->singlerow;
	mstate->keyparamids = node->keyparamids;

	/*
	 * Record if the cache keys should be compared bit by bit, or logically
	 * using the type's hash equality operator
	 */
	mstate->binary_mode = node->binary_mode;

	/* Zero the statistics counters */
	memset(&mstate->stats, 0, sizeof(MemoizeInstrumentation));

	/* Allocate and set up the actual cache */
	build_hash_table(mstate, node->est_entries);

	return mstate;
}

void
ExecEndMemoize(MemoizeState *node)
{
#ifdef USE_ASSERT_CHECKING
	/* Validate the memory accounting code is correct in assert builds. */
	{
		int			count;
		uint64		mem = 0;
		memoize_iterator i;
		MemoizeEntry *entry;

		memoize_start_iterate(node->hashtable, &i);

		count = 0;
		while ((entry = memoize_iterate(node->hashtable, &i)) != NULL)
		{
			MemoizeTuple *tuple = entry->tuplehead;

			mem += EMPTY_ENTRY_MEMORY_BYTES(entry);
			while (tuple != NULL)
			{
				mem += CACHE_TUPLE_BYTES(tuple);
				tuple = tuple->next;
			}
			count++;
		}

		Assert(count == node->hashtable->members);
		Assert(mem == node->mem_used);
	}
#endif

	/*
	 * When ending a parallel worker, copy the statistics gathered by the
	 * worker back into shared memory so that it can be picked up by the main
	 * process to report in EXPLAIN ANALYZE.
	 */
	if (node->shared_info != NULL && IsParallelWorker())
	{
		MemoizeInstrumentation *si;

		/* Make mem_peak available for EXPLAIN */
		if (node->stats.mem_peak == 0)
			node->stats.mem_peak = node->mem_used;

		Assert(ParallelWorkerNumber <= node->shared_info->num_workers);
		si = &node->shared_info->sinstrument[ParallelWorkerNumber];
		memcpy(si, &node->stats, sizeof(MemoizeInstrumentation));
	}

	/* Remove the cache context */
	MemoryContextDelete(node->tableContext);

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	/* must drop pointer to cache result tuple */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	/*
	 * free exprcontext
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * shut down the subplan
	 */
	ExecEndNode(outerPlanState(node));
}

void
ExecReScanMemoize(MemoizeState *node)
{
	PlanState  *outerPlan = outerPlanState(node);

	/* Mark that we must lookup the cache for a new set of parameters */
	node->mstatus = MEMO_CACHE_LOOKUP;

	/* nullify pointers used for the last scan */
	node->entry = NULL;
	node->last_tuple = NULL;

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);

	/*
	 * Purge the entire cache if a parameter changed that is not part of the
	 * cache key.
	 */
	if (bms_nonempty_difference(outerPlan->chgParam, node->keyparamids))
		cache_purge_all(node);
}

/*
 * ExecEstimateCacheEntryOverheadBytes
 *		For use in the query planner to help it estimate the amount of memory
 *		required to store a single entry in the cache.
 */
double
ExecEstimateCacheEntryOverheadBytes(double ntuples)
{
	return sizeof(MemoizeEntry) + sizeof(MemoizeKey) + sizeof(MemoizeTuple) *
		ntuples;
}

/* ----------------------------------------------------------------
 *						Parallel Query Support
 * ----------------------------------------------------------------
 */

 /* ----------------------------------------------------------------
  *		ExecMemoizeEstimate
  *
  *		Estimate space required to propagate memoize statistics.
  * ----------------------------------------------------------------
  */
void
ExecMemoizeEstimate(MemoizeState *node, ParallelContext *pcxt)
{
	Size		size;

	/* don't need this if not instrumenting or no workers */
	if (!node->ss.ps.instrument || pcxt->nworkers == 0)
		return;

	size = mul_size(pcxt->nworkers, sizeof(MemoizeInstrumentation));
	size = add_size(size, offsetof(SharedMemoizeInfo, sinstrument));
	shm_toc_estimate_chunk(&pcxt->estimator, size);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}

/* ----------------------------------------------------------------
 *		ExecMemoizeInitializeDSM
 *
 *		Initialize DSM space for memoize statistics.
 * ----------------------------------------------------------------
 */
void
ExecMemoizeInitializeDSM(MemoizeState *node, ParallelContext *pcxt)
{
	Size		size;

	/* don't need this if not instrumenting or no workers */
	if (!node->ss.ps.instrument || pcxt->nworkers == 0)
		return;

	size = offsetof(SharedMemoizeInfo, sinstrument)
		+ pcxt->nworkers * sizeof(MemoizeInstrumentation);
	node->shared_info = shm_toc_allocate(pcxt->toc, size);
	/* ensure any unfilled slots will contain zeroes */
	memset(node->shared_info, 0, size);
	node->shared_info->num_workers = pcxt->nworkers;
	shm_toc_insert(pcxt->toc, node->ss.ps.plan->plan_node_id,
				   node->shared_info);
}

/* ----------------------------------------------------------------
 *		ExecMemoizeInitializeWorker
 *
 *		Attach worker to DSM space for memoize statistics.
 * ----------------------------------------------------------------
 */
void
ExecMemoizeInitializeWorker(MemoizeState *node, ParallelWorkerContext *pwcxt)
{
	node->shared_info =
		shm_toc_lookup(pwcxt->toc, node->ss.ps.plan->plan_node_id, true);
}

/* ----------------------------------------------------------------
 *		ExecMemoizeRetrieveInstrumentation
 *
 *		Transfer memoize statistics from DSM to private memory.
 * ----------------------------------------------------------------
 */
void
ExecMemoizeRetrieveInstrumentation(MemoizeState *node)
{
	Size		size;
	SharedMemoizeInfo *si;

	if (node->shared_info == NULL)
		return;

	size = offsetof(SharedMemoizeInfo, sinstrument)
		+ node->shared_info->num_workers * sizeof(MemoizeInstrumentation);
	si = palloc(size);
	memcpy(si, node->shared_info, size);
	node->shared_info = si;
}
