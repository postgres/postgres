/*-------------------------------------------------------------------------
 *
 * ginbulk.c
 *	  routines for fast build of inverted index
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			$PostgreSQL: pgsql/src/backend/access/gin/ginbulk.c,v 1.19.4.1 2010/08/01 02:12:51 tgl Exp $
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin.h"
#include "utils/datum.h"
#include "utils/memutils.h"


#define DEF_NENTRY	2048		/* EntryAccumulator allocation quantum */
#define DEF_NPTR	5			/* ItemPointer initial allocation quantum */


/* Combiner function for rbtree.c */
static void
ginCombineData(RBNode *existing, const RBNode *newdata, void *arg)
{
	EntryAccumulator *eo = (EntryAccumulator *) existing;
	const EntryAccumulator *en = (const EntryAccumulator *) newdata;
	BuildAccumulator *accum = (BuildAccumulator *) arg;

	/*
	 * Note this code assumes that newdata contains only one itempointer.
	 */
	if (eo->number >= eo->length)
	{
		accum->allocatedMemory -= GetMemoryChunkSpace(eo->list);
		eo->length *= 2;
		eo->list = (ItemPointerData *) repalloc(eo->list,
									   sizeof(ItemPointerData) * eo->length);
		accum->allocatedMemory += GetMemoryChunkSpace(eo->list);
	}

	/* If item pointers are not ordered, they will need to be sorted. */
	if (eo->shouldSort == FALSE)
	{
		int			res;

		res = compareItemPointers(eo->list + eo->number - 1, en->list);
		Assert(res != 0);

		if (res > 0)
			eo->shouldSort = TRUE;
	}

	eo->list[eo->number] = en->list[0];
	eo->number++;
}

/* Comparator function for rbtree.c */
static int
cmpEntryAccumulator(const RBNode *a, const RBNode *b, void *arg)
{
	const EntryAccumulator *ea = (const EntryAccumulator *) a;
	const EntryAccumulator *eb = (const EntryAccumulator *) b;
	BuildAccumulator *accum = (BuildAccumulator *) arg;

	return compareAttEntries(accum->ginstate, ea->attnum, ea->value,
							 eb->attnum, eb->value);
}

/* Allocator function for rbtree.c */
static RBNode *
ginAllocEntryAccumulator(void *arg)
{
	BuildAccumulator *accum = (BuildAccumulator *) arg;
	EntryAccumulator *ea;

	/*
	 * Allocate memory by rather big chunks to decrease overhead.  We have
	 * no need to reclaim RBNodes individually, so this costs nothing.
	 */
	if (accum->entryallocator == NULL || accum->length >= DEF_NENTRY)
	{
		accum->entryallocator = palloc(sizeof(EntryAccumulator) * DEF_NENTRY);
		accum->allocatedMemory += GetMemoryChunkSpace(accum->entryallocator);
		accum->length = 0;
	}

	/* Allocate new RBNode from current chunk */
	ea = accum->entryallocator + accum->length;
	accum->length++;

	return (RBNode *) ea;
}

void
ginInitBA(BuildAccumulator *accum)
{
	accum->allocatedMemory = 0;
	accum->length = 0;
	accum->entryallocator = NULL;
	accum->tree = rb_create(sizeof(EntryAccumulator),
							cmpEntryAccumulator,
							ginCombineData,
							ginAllocEntryAccumulator,
							NULL,				/* no freefunc needed */
							(void *) accum);
}

/*
 * This is basically the same as datumCopy(), but modified to count
 * palloc'd space in accum.
 */
static Datum
getDatumCopy(BuildAccumulator *accum, OffsetNumber attnum, Datum value)
{
	Form_pg_attribute att = accum->ginstate->origTupdesc->attrs[attnum - 1];
	Datum		res;

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
ginInsertEntry(BuildAccumulator *accum, ItemPointer heapptr, OffsetNumber attnum, Datum entry)
{
	EntryAccumulator key;
	EntryAccumulator *ea;
	bool		isNew;

	/*
	 * For the moment, fill only the fields of key that will be looked at
	 * by cmpEntryAccumulator or ginCombineData.
	 */
	key.attnum = attnum;
	key.value = entry;
	/* temporarily set up single-entry itempointer list */
	key.list = heapptr;

	ea = (EntryAccumulator *) rb_insert(accum->tree, (RBNode *) &key, &isNew);

	if (isNew)
	{
		/*
		 * Finish initializing new tree entry, including making permanent
		 * copies of the datum and itempointer.
		 */
		ea->value = getDatumCopy(accum, attnum, entry);
		ea->length = DEF_NPTR;
		ea->number = 1;
		ea->shouldSort = FALSE;
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
 * Insert one heap pointer.
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
 * tree; then, we insert the middles of each half of out virtual array, then
 * middles of quarters, etc.
 */
void
ginInsertRecordBA(BuildAccumulator *accum, ItemPointer heapptr, OffsetNumber attnum,
				  Datum *entries, int32 nentry)
{
	uint32		step = nentry;

	if (nentry <= 0)
		return;

	Assert(ItemPointerIsValid(heapptr) && attnum >= FirstOffsetNumber);

	/*
	 * step will contain largest power of 2 and <= nentry
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

		for (i = step - 1; i < nentry && i >= 0; i += step << 1 /* *2 */ )
			ginInsertEntry(accum, heapptr, attnum, entries[i]);

		step >>= 1;				/* /2 */
	}
}

static int
qsortCompareItemPointers(const void *a, const void *b)
{
	int			res = compareItemPointers((ItemPointer) a, (ItemPointer) b);

	Assert(res != 0);
	return res;
}

/* Prepare to read out the rbtree contents using ginGetEntry */
void
ginBeginBAScan(BuildAccumulator *accum)
{
	rb_begin_iterate(accum->tree, LeftRightWalk);
}

ItemPointerData *
ginGetEntry(BuildAccumulator *accum, OffsetNumber *attnum, Datum *value, uint32 *n)
{
	EntryAccumulator *entry;
	ItemPointerData *list;

	entry = (EntryAccumulator *) rb_iterate(accum->tree);

	if (entry == NULL)
		return NULL;

	*n = entry->number;
	*attnum = entry->attnum;
	*value = entry->value;
	list = entry->list;

	Assert(list != NULL);

	if (entry->shouldSort && entry->number > 1)
		qsort(list, *n, sizeof(ItemPointerData), qsortCompareItemPointers);

	return list;
}
