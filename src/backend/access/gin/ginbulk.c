/*-------------------------------------------------------------------------
 *
 * ginbulk.c
 *	  routines for fast build of inverted index
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginbulk.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "access/gin_private.h"
#include "utils/datum.h"
#include "utils/memutils.h"


#define DEF_NENTRY	2048		/* GinEntryAccumulator allocation quantum */
#define DEF_NPTR	5			/* ItemPointer initial allocation quantum */


/* Combiner function for rbtree.c */
static void
ginCombineData(RBTNode *existing, const RBTNode *newdata, void *arg)
{
	GinEntryAccumulator *eo = (GinEntryAccumulator *) existing;
	const GinEntryAccumulator *en = (const GinEntryAccumulator *) newdata;
	BuildAccumulator *accum = (BuildAccumulator *) arg;

	/*
	 * Note this code assumes that newdata contains only one itempointer.
	 */
	if (eo->count >= eo->maxcount)
	{
		if (eo->maxcount > INT_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("posting list is too long"),
					 errhint("Reduce maintenance_work_mem.")));

		accum->allocatedMemory -= GetMemoryChunkSpace(eo->list);
		eo->maxcount *= 2;
		eo->list = (ItemPointerData *)
			repalloc_huge(eo->list, sizeof(ItemPointerData) * eo->maxcount);
		accum->allocatedMemory += GetMemoryChunkSpace(eo->list);
	}

	/* If item pointers are not ordered, they will need to be sorted later */
	if (eo->shouldSort == false)
	{
		int			res;

		res = ginCompareItemPointers(eo->list + eo->count - 1, en->list);
		Assert(res != 0);

		if (res > 0)
			eo->shouldSort = true;
	}

	eo->list[eo->count] = en->list[0];
	eo->count++;
}

/* Comparator function for rbtree.c */
static int
cmpEntryAccumulator(const RBTNode *a, const RBTNode *b, void *arg)
{
	const GinEntryAccumulator *ea = (const GinEntryAccumulator *) a;
	const GinEntryAccumulator *eb = (const GinEntryAccumulator *) b;
	BuildAccumulator *accum = (BuildAccumulator *) arg;

	return ginCompareAttEntries(accum->ginstate,
								ea->attnum, ea->key, ea->category,
								eb->attnum, eb->key, eb->category);
}

/* Allocator function for rbtree.c */
static RBTNode *
ginAllocEntryAccumulator(void *arg)
{
	BuildAccumulator *accum = (BuildAccumulator *) arg;
	GinEntryAccumulator *ea;

	/*
	 * Allocate memory by rather big chunks to decrease overhead.  We have no
	 * need to reclaim RBTNodes individually, so this costs nothing.
	 */
	if (accum->entryallocator == NULL || accum->eas_used >= DEF_NENTRY)
	{
		accum->entryallocator = palloc(sizeof(GinEntryAccumulator) * DEF_NENTRY);
		accum->allocatedMemory += GetMemoryChunkSpace(accum->entryallocator);
		accum->eas_used = 0;
	}

	/* Allocate new RBTNode from current chunk */
	ea = accum->entryallocator + accum->eas_used;
	accum->eas_used++;

	return (RBTNode *) ea;
}

void
ginInitBA(BuildAccumulator *accum)
{
	/* accum->ginstate is intentionally not set here */
	accum->allocatedMemory = 0;
	accum->entryallocator = NULL;
	accum->eas_used = 0;
	accum->tree = rbt_create(sizeof(GinEntryAccumulator),
							 cmpEntryAccumulator,
							 ginCombineData,
							 ginAllocEntryAccumulator,
							 NULL,	/* no freefunc needed */
							 (void *) accum);
}

/*
 * This is basically the same as datumCopy(), but extended to count
 * palloc'd space in accum->allocatedMemory.
 */
static Datum
getDatumCopy(BuildAccumulator *accum, OffsetNumber attnum, Datum value)
{
	Form_pg_attribute att;
	Datum		res;

	att = TupleDescAttr(accum->ginstate->origTupdesc, attnum - 1);
	if (att->attbyval)
		res = value;
	else
	{
		res = datumCopy(value, false, att->attlen);
		accum->allocatedMemory += GetMemoryChunkSpace(DatumGetPointer(res));
	}
	return res;
}

/*
 * Find/store one entry from indexed value.
 */
static void
ginInsertBAEntry(BuildAccumulator *accum,
				 ItemPointer heapptr, OffsetNumber attnum,
				 Datum key, GinNullCategory category)
{
	GinEntryAccumulator eatmp;
	GinEntryAccumulator *ea;
	bool		isNew;

	/*
	 * For the moment, fill only the fields of eatmp that will be looked at by
	 * cmpEntryAccumulator or ginCombineData.
	 */
	eatmp.attnum = attnum;
	eatmp.key = key;
	eatmp.category = category;
	/* temporarily set up single-entry itempointer list */
	eatmp.list = heapptr;

	ea = (GinEntryAccumulator *) rbt_insert(accum->tree, (RBTNode *) &eatmp,
											&isNew);

	if (isNew)
	{
		/*
		 * Finish initializing new tree entry, including making permanent
		 * copies of the datum (if it's not null) and itempointer.
		 */
		if (category == GIN_CAT_NORM_KEY)
			ea->key = getDatumCopy(accum, attnum, key);
		ea->maxcount = DEF_NPTR;
		ea->count = 1;
		ea->shouldSort = false;
		ea->list =
			(ItemPointerData *) palloc(sizeof(ItemPointerData) * DEF_NPTR);
		ea->list[0] = *heapptr;
		accum->allocatedMemory += GetMemoryChunkSpace(ea->list);
	}
	else
	{
		/*
		 * ginCombineData did everything needed.
		 */
	}
}

/*
 * Insert the entries for one heap pointer.
 *
 * Since the entries are being inserted into a balanced binary tree, you
 * might think that the order of insertion wouldn't be critical, but it turns
 * out that inserting the entries in sorted order results in a lot of
 * rebalancing operations and is slow.  To prevent this, we attempt to insert
 * the nodes in an order that will produce a nearly-balanced tree if the input
 * is in fact sorted.
 *
 * We do this as follows.  First, we imagine that we have an array whose size
 * is the smallest power of two greater than or equal to the actual array
 * size.  Second, we insert the middle entry of our virtual array into the
 * tree; then, we insert the middles of each half of our virtual array, then
 * middles of quarters, etc.
 */
void
ginInsertBAEntries(BuildAccumulator *accum,
				   ItemPointer heapptr, OffsetNumber attnum,
				   Datum *entries, GinNullCategory *categories,
				   int32 nentries)
{
	uint32		step = nentries;

	if (nentries <= 0)
		return;

	Assert(ItemPointerIsValid(heapptr) && attnum >= FirstOffsetNumber);

	/*
	 * step will contain largest power of 2 and <= nentries
	 */
	step |= (step >> 1);
	step |= (step >> 2);
	step |= (step >> 4);
	step |= (step >> 8);
	step |= (step >> 16);
	step >>= 1;
	step++;

	while (step > 0)
	{
		int			i;

		for (i = step - 1; i < nentries && i >= 0; i += step << 1 /* *2 */ )
			ginInsertBAEntry(accum, heapptr, attnum,
							 entries[i], categories[i]);

		step >>= 1;				/* /2 */
	}
}

static int
qsortCompareItemPointers(const void *a, const void *b)
{
	int			res = ginCompareItemPointers((ItemPointer) a, (ItemPointer) b);

	/* Assert that there are no equal item pointers being sorted */
	Assert(res != 0);
	return res;
}

/* Prepare to read out the rbtree contents using ginGetBAEntry */
void
ginBeginBAScan(BuildAccumulator *accum)
{
	rbt_begin_iterate(accum->tree, LeftRightWalk, &accum->tree_walk);
}

/*
 * Get the next entry in sequence from the BuildAccumulator's rbtree.
 * This consists of a single key datum and a list (array) of one or more
 * heap TIDs in which that key is found.  The list is guaranteed sorted.
 */
ItemPointerData *
ginGetBAEntry(BuildAccumulator *accum,
			  OffsetNumber *attnum, Datum *key, GinNullCategory *category,
			  uint32 *n)
{
	GinEntryAccumulator *entry;
	ItemPointerData *list;

	entry = (GinEntryAccumulator *) rbt_iterate(&accum->tree_walk);

	if (entry == NULL)
		return NULL;			/* no more entries */

	*attnum = entry->attnum;
	*key = entry->key;
	*category = entry->category;
	list = entry->list;
	*n = entry->count;

	Assert(list != NULL && entry->count > 0);

	if (entry->shouldSort && entry->count > 1)
		qsort(list, entry->count, sizeof(ItemPointerData),
			  qsortCompareItemPointers);

	return list;
}
