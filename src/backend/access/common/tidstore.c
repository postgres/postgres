/*-------------------------------------------------------------------------
 *
 * tidstore.c
 *		TID (ItemPointerData) storage implementation.
 *
 * TidStore is a in-memory data structure to store TIDs (ItemPointerData).
 * Internally it uses a radix tree as the storage for TIDs. The key is the
 * BlockNumber and the value is a bitmap of offsets, BlocktableEntry.
 *
 * TidStore can be shared among parallel worker processes by using
 * TidStoreCreateShared(). Other backends can attach to the shared TidStore
 * by TidStoreAttach().
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/common/tidstore.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tidstore.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "storage/lwlock.h"
#include "utils/dsa.h"


#define WORDNUM(x)	((x) / BITS_PER_BITMAPWORD)
#define BITNUM(x)	((x) % BITS_PER_BITMAPWORD)

/* number of active words for a page: */
#define WORDS_PER_PAGE(n) ((n) / BITS_PER_BITMAPWORD + 1)

/* number of offsets we can store in the header of a BlocktableEntry */
#define NUM_FULL_OFFSETS ((sizeof(uintptr_t) - sizeof(uint8) - sizeof(int8)) / sizeof(OffsetNumber))

/*
 * This is named similarly to PagetableEntry in tidbitmap.c
 * because the two have a similar function.
 */
typedef struct BlocktableEntry
{
	struct
	{
#ifndef WORDS_BIGENDIAN
		/*
		 * We need to position this member to reserve space for the backing
		 * radix tree to tag the lowest bit when struct 'header' is stored
		 * inside a pointer or DSA pointer.
		 */
		uint8		flags;

		int8		nwords;
#endif

		/*
		 * We can store a small number of offsets here to avoid wasting space
		 * with a sparse bitmap.
		 */
		OffsetNumber full_offsets[NUM_FULL_OFFSETS];

#ifdef WORDS_BIGENDIAN
		int8		nwords;
		uint8		flags;
#endif
	}			header;

	/*
	 * We don't expect any padding space here, but to be cautious, code
	 * creating new entries should zero out space up to 'words'.
	 */

	bitmapword	words[FLEXIBLE_ARRAY_MEMBER];
} BlocktableEntry;

/*
 * The type of 'nwords' limits the max number of words in the 'words' array.
 * This computes the max offset we can actually store in the bitmap. In
 * practice, it's almost always the same as MaxOffsetNumber.
 */
#define MAX_OFFSET_IN_BITMAP Min(BITS_PER_BITMAPWORD * PG_INT8_MAX - 1, MaxOffsetNumber)

#define MaxBlocktableEntrySize \
	offsetof(BlocktableEntry, words) + \
		(sizeof(bitmapword) * WORDS_PER_PAGE(MAX_OFFSET_IN_BITMAP))

#define RT_PREFIX local_ts
#define RT_SCOPE static
#define RT_DECLARE
#define RT_DEFINE
#define RT_VALUE_TYPE BlocktableEntry
#define RT_VARLEN_VALUE_SIZE(page) \
	(offsetof(BlocktableEntry, words) + \
	sizeof(bitmapword) * (page)->header.nwords)
#define RT_RUNTIME_EMBEDDABLE_VALUE
#include "lib/radixtree.h"

#define RT_PREFIX shared_ts
#define RT_SHMEM
#define RT_SCOPE static
#define RT_DECLARE
#define RT_DEFINE
#define RT_VALUE_TYPE BlocktableEntry
#define RT_VARLEN_VALUE_SIZE(page) \
	(offsetof(BlocktableEntry, words) + \
	sizeof(bitmapword) * (page)->header.nwords)
#define RT_RUNTIME_EMBEDDABLE_VALUE
#include "lib/radixtree.h"

/* Per-backend state for a TidStore */
struct TidStore
{
	/* MemoryContext where the TidStore is allocated */
	MemoryContext context;

	/* MemoryContext that the radix tree uses */
	MemoryContext rt_context;

	/* Storage for TIDs. Use either one depending on TidStoreIsShared() */
	union
	{
		local_ts_radix_tree *local;
		shared_ts_radix_tree *shared;
	}			tree;

	/* DSA area for TidStore if using shared memory */
	dsa_area   *area;
};
#define TidStoreIsShared(ts) ((ts)->area != NULL)

/* Iterator for TidStore */
struct TidStoreIter
{
	TidStore   *ts;

	/* iterator of radix tree. Use either one depending on TidStoreIsShared() */
	union
	{
		shared_ts_iter *shared;
		local_ts_iter *local;
	}			tree_iter;

	/* output for the caller */
	TidStoreIterResult output;
};

/*
 * Create a TidStore. The TidStore will live in the memory context that is
 * CurrentMemoryContext at the time of this call. The TID storage, backed
 * by a radix tree, will live in its child memory context, rt_context.
 *
 * "max_bytes" is not an internally-enforced limit; it is used only as a
 * hint to cap the memory block size of the memory context for TID storage.
 * This reduces space wastage due to over-allocation. If the caller wants to
 * monitor memory usage, it must compare its limit with the value reported
 * by TidStoreMemoryUsage().
 */
TidStore *
TidStoreCreateLocal(size_t max_bytes, bool insert_only)
{
	TidStore   *ts;
	size_t		initBlockSize = ALLOCSET_DEFAULT_INITSIZE;
	size_t		minContextSize = ALLOCSET_DEFAULT_MINSIZE;
	size_t		maxBlockSize = ALLOCSET_DEFAULT_MAXSIZE;

	ts = palloc0(sizeof(TidStore));
	ts->context = CurrentMemoryContext;

	/* choose the maxBlockSize to be no larger than 1/16 of max_bytes */
	while (16 * maxBlockSize > max_bytes)
		maxBlockSize >>= 1;

	if (maxBlockSize < ALLOCSET_DEFAULT_INITSIZE)
		maxBlockSize = ALLOCSET_DEFAULT_INITSIZE;

	/* Create a memory context for the TID storage */
	if (insert_only)
	{
		ts->rt_context = BumpContextCreate(CurrentMemoryContext,
										   "TID storage",
										   minContextSize,
										   initBlockSize,
										   maxBlockSize);
	}
	else
	{
		ts->rt_context = AllocSetContextCreate(CurrentMemoryContext,
											   "TID storage",
											   minContextSize,
											   initBlockSize,
											   maxBlockSize);
	}

	ts->tree.local = local_ts_create(ts->rt_context);

	return ts;
}

/*
 * Similar to TidStoreCreateLocal() but create a shared TidStore on a
 * DSA area. The TID storage will live in the DSA area, and the memory
 * context rt_context will have only meta data of the radix tree.
 *
 * The returned object is allocated in backend-local memory.
 */
TidStore *
TidStoreCreateShared(size_t max_bytes, int tranche_id)
{
	TidStore   *ts;
	dsa_area   *area;
	size_t		dsa_init_size = DSA_DEFAULT_INIT_SEGMENT_SIZE;
	size_t		dsa_max_size = DSA_MAX_SEGMENT_SIZE;

	ts = palloc0(sizeof(TidStore));
	ts->context = CurrentMemoryContext;

	ts->rt_context = AllocSetContextCreate(CurrentMemoryContext,
										   "TID storage meta data",
										   ALLOCSET_SMALL_SIZES);

	/*
	 * Choose the initial and maximum DSA segment sizes to be no longer than
	 * 1/8 of max_bytes.
	 */
	while (8 * dsa_max_size > max_bytes)
		dsa_max_size >>= 1;

	if (dsa_max_size < DSA_MIN_SEGMENT_SIZE)
		dsa_max_size = DSA_MIN_SEGMENT_SIZE;

	if (dsa_init_size > dsa_max_size)
		dsa_init_size = dsa_max_size;

	area = dsa_create_ext(tranche_id, dsa_init_size, dsa_max_size);
	ts->tree.shared = shared_ts_create(ts->rt_context, area,
									   tranche_id);
	ts->area = area;

	return ts;
}

/*
 * Attach to the shared TidStore. 'area_handle' is the DSA handle where
 * the TidStore is created. 'handle' is the dsa_pointer returned by
 * TidStoreGetHandle(). The returned object is allocated in backend-local
 * memory using the CurrentMemoryContext.
 */
TidStore *
TidStoreAttach(dsa_handle area_handle, dsa_pointer handle)
{
	TidStore   *ts;
	dsa_area   *area;

	Assert(area_handle != DSA_HANDLE_INVALID);
	Assert(DsaPointerIsValid(handle));

	/* create per-backend state */
	ts = palloc0(sizeof(TidStore));

	area = dsa_attach(area_handle);

	/* Find the shared the shared radix tree */
	ts->tree.shared = shared_ts_attach(area, handle);
	ts->area = area;

	return ts;
}

/*
 * Detach from a TidStore. This also detaches from radix tree and frees
 * the backend-local resources.
 */
void
TidStoreDetach(TidStore *ts)
{
	Assert(TidStoreIsShared(ts));

	shared_ts_detach(ts->tree.shared);
	dsa_detach(ts->area);

	pfree(ts);
}

/*
 * Lock support functions.
 *
 * We can use the radix tree's lock for shared TidStore as the data we
 * need to protect is only the shared radix tree.
 */

void
TidStoreLockExclusive(TidStore *ts)
{
	if (TidStoreIsShared(ts))
		shared_ts_lock_exclusive(ts->tree.shared);
}

void
TidStoreLockShare(TidStore *ts)
{
	if (TidStoreIsShared(ts))
		shared_ts_lock_share(ts->tree.shared);
}

void
TidStoreUnlock(TidStore *ts)
{
	if (TidStoreIsShared(ts))
		shared_ts_unlock(ts->tree.shared);
}

/*
 * Destroy a TidStore, returning all memory.
 *
 * Note that the caller must be certain that no other backend will attempt to
 * access the TidStore before calling this function. Other backend must
 * explicitly call TidStoreDetach() to free up backend-local memory associated
 * with the TidStore. The backend that calls TidStoreDestroy() must not call
 * TidStoreDetach().
 */
void
TidStoreDestroy(TidStore *ts)
{
	/* Destroy underlying radix tree */
	if (TidStoreIsShared(ts))
	{
		shared_ts_free(ts->tree.shared);

		dsa_detach(ts->area);
	}
	else
		local_ts_free(ts->tree.local);

	MemoryContextDelete(ts->rt_context);

	pfree(ts);
}

/*
 * Create or replace an entry for the given block and array of offsets.
 *
 * NB: This function is designed and optimized for vacuum's heap scanning
 * phase, so has some limitations:
 *
 * - The offset numbers "offsets" must be sorted in ascending order.
 * - If the block number already exists, the entry will be replaced --
 *	 there is no way to add or remove offsets from an entry.
 */
void
TidStoreSetBlockOffsets(TidStore *ts, BlockNumber blkno, OffsetNumber *offsets,
						int num_offsets)
{
	union
	{
		char		data[MaxBlocktableEntrySize];
		BlocktableEntry force_align_entry;
	}			data;
	BlocktableEntry *page = (BlocktableEntry *) data.data;
	bitmapword	word;
	int			wordnum;
	int			next_word_threshold;
	int			idx = 0;

	Assert(num_offsets > 0);

	/* Check if the given offset numbers are ordered */
	for (int i = 1; i < num_offsets; i++)
		Assert(offsets[i] > offsets[i - 1]);

	memset(page, 0, offsetof(BlocktableEntry, words));

	if (num_offsets <= NUM_FULL_OFFSETS)
	{
		for (int i = 0; i < num_offsets; i++)
		{
			OffsetNumber off = offsets[i];

			/* safety check to ensure we don't overrun bit array bounds */
			if (off == InvalidOffsetNumber || off > MAX_OFFSET_IN_BITMAP)
				elog(ERROR, "tuple offset out of range: %u", off);

			page->header.full_offsets[i] = off;
		}

		page->header.nwords = 0;
	}
	else
	{
		for (wordnum = 0, next_word_threshold = BITS_PER_BITMAPWORD;
			 wordnum <= WORDNUM(offsets[num_offsets - 1]);
			 wordnum++, next_word_threshold += BITS_PER_BITMAPWORD)
		{
			word = 0;

			while (idx < num_offsets)
			{
				OffsetNumber off = offsets[idx];

				/* safety check to ensure we don't overrun bit array bounds */
				if (off == InvalidOffsetNumber || off > MAX_OFFSET_IN_BITMAP)
					elog(ERROR, "tuple offset out of range: %u", off);

				if (off >= next_word_threshold)
					break;

				word |= ((bitmapword) 1 << BITNUM(off));
				idx++;
			}

			/* write out offset bitmap for this wordnum */
			page->words[wordnum] = word;
		}

		page->header.nwords = wordnum;
		Assert(page->header.nwords == WORDS_PER_PAGE(offsets[num_offsets - 1]));
	}

	if (TidStoreIsShared(ts))
		shared_ts_set(ts->tree.shared, blkno, page);
	else
		local_ts_set(ts->tree.local, blkno, page);
}

/* Return true if the given TID is present in the TidStore */
bool
TidStoreIsMember(TidStore *ts, ItemPointer tid)
{
	int			wordnum;
	int			bitnum;
	BlocktableEntry *page;
	BlockNumber blk = ItemPointerGetBlockNumber(tid);
	OffsetNumber off = ItemPointerGetOffsetNumber(tid);

	if (TidStoreIsShared(ts))
		page = shared_ts_find(ts->tree.shared, blk);
	else
		page = local_ts_find(ts->tree.local, blk);

	/* no entry for the blk */
	if (page == NULL)
		return false;

	if (page->header.nwords == 0)
	{
		/* we have offsets in the header */
		for (int i = 0; i < NUM_FULL_OFFSETS; i++)
		{
			if (page->header.full_offsets[i] == off)
				return true;
		}
		return false;
	}
	else
	{
		wordnum = WORDNUM(off);
		bitnum = BITNUM(off);

		/* no bitmap for the off */
		if (wordnum >= page->header.nwords)
			return false;

		return (page->words[wordnum] & ((bitmapword) 1 << bitnum)) != 0;
	}
}

/*
 * Prepare to iterate through a TidStore.
 *
 * The TidStoreIter struct is created in the caller's memory context, and it
 * will be freed in TidStoreEndIterate.
 *
 * The caller is responsible for locking TidStore until the iteration is
 * finished.
 */
TidStoreIter *
TidStoreBeginIterate(TidStore *ts)
{
	TidStoreIter *iter;

	iter = palloc0(sizeof(TidStoreIter));
	iter->ts = ts;

	if (TidStoreIsShared(ts))
		iter->tree_iter.shared = shared_ts_begin_iterate(ts->tree.shared);
	else
		iter->tree_iter.local = local_ts_begin_iterate(ts->tree.local);

	return iter;
}


/*
 * Return a result that contains the next block number and that can be used to
 * obtain the set of offsets by calling TidStoreGetBlockOffsets().  The result
 * is copyable.
 */
TidStoreIterResult *
TidStoreIterateNext(TidStoreIter *iter)
{
	uint64		key;
	BlocktableEntry *page;

	if (TidStoreIsShared(iter->ts))
		page = shared_ts_iterate_next(iter->tree_iter.shared, &key);
	else
		page = local_ts_iterate_next(iter->tree_iter.local, &key);

	if (page == NULL)
		return NULL;

	iter->output.blkno = key;
	iter->output.internal_page = page;

	return &(iter->output);
}

/*
 * Finish the iteration on TidStore.
 *
 * The caller is responsible for releasing any locks.
 */
void
TidStoreEndIterate(TidStoreIter *iter)
{
	if (TidStoreIsShared(iter->ts))
		shared_ts_end_iterate(iter->tree_iter.shared);
	else
		local_ts_end_iterate(iter->tree_iter.local);

	pfree(iter);
}

/*
 * Return the memory usage of TidStore.
 */
size_t
TidStoreMemoryUsage(TidStore *ts)
{
	if (TidStoreIsShared(ts))
		return shared_ts_memory_usage(ts->tree.shared);
	else
		return local_ts_memory_usage(ts->tree.local);
}

/*
 * Return the DSA area where the TidStore lives.
 */
dsa_area *
TidStoreGetDSA(TidStore *ts)
{
	Assert(TidStoreIsShared(ts));

	return ts->area;
}

dsa_pointer
TidStoreGetHandle(TidStore *ts)
{
	Assert(TidStoreIsShared(ts));

	return (dsa_pointer) shared_ts_get_handle(ts->tree.shared);
}

/*
 * Given a TidStoreIterResult returned by TidStoreIterateNext(), extract the
 * offset numbers.  Returns the number of offsets filled in, if <=
 * max_offsets.  Otherwise, fills in as much as it can in the given space, and
 * returns the size of the buffer that would be needed.
 */
int
TidStoreGetBlockOffsets(TidStoreIterResult *result,
						OffsetNumber *offsets,
						int max_offsets)
{
	BlocktableEntry *page = result->internal_page;
	int			num_offsets = 0;
	int			wordnum;

	if (page->header.nwords == 0)
	{
		/* we have offsets in the header */
		for (int i = 0; i < NUM_FULL_OFFSETS; i++)
		{
			if (page->header.full_offsets[i] != InvalidOffsetNumber)
			{
				if (num_offsets < max_offsets)
					offsets[num_offsets] = page->header.full_offsets[i];
				num_offsets++;
			}
		}
	}
	else
	{
		for (wordnum = 0; wordnum < page->header.nwords; wordnum++)
		{
			bitmapword	w = page->words[wordnum];
			int			off = wordnum * BITS_PER_BITMAPWORD;

			while (w != 0)
			{
				if (w & 1)
				{
					if (num_offsets < max_offsets)
						offsets[num_offsets] = (OffsetNumber) off;
					num_offsets++;
				}
				off++;
				w >>= 1;
			}
		}
	}

	return num_offsets;
}
