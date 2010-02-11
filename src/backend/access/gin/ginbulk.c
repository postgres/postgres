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
 *			$PostgreSQL: pgsql/src/backend/access/gin/ginbulk.c,v 1.18 2010/02/11 14:29:50 teodor Exp $
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin.h"
#include "utils/datum.h"
#include "utils/memutils.h"


#define DEF_NENTRY	2048
#define DEF_NPTR	4

static void*
ginAppendData(void *old, void *new, void *arg)
{
	EntryAccumulator	*eo = (EntryAccumulator*)old,
						*en = (EntryAccumulator*)new;

	BuildAccumulator	*accum = (BuildAccumulator*)arg;

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

	return old;
}

static int
cmpEntryAccumulator(const void *a, const void *b, void *arg)
{
	EntryAccumulator	*ea = (EntryAccumulator*)a;
	EntryAccumulator	*eb = (EntryAccumulator*)b;
	BuildAccumulator	*accum = (BuildAccumulator*)arg;

	return compareAttEntries(accum->ginstate, ea->attnum, ea->value,
							 eb->attnum, eb->value);
}

void
ginInitBA(BuildAccumulator *accum)
{
	accum->allocatedMemory = 0;
	accum->entryallocator = NULL;
	accum->tree = rb_create(cmpEntryAccumulator, ginAppendData, NULL, accum);
	accum->iterator = NULL;
	accum->tmpList = NULL;
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
	EntryAccumulator 	*key,
						*ea;

	/* 
	 * Allocate memory by rather big chunk to decrease overhead, we don't
	 * keep pointer to previously allocated chunks because they will free
	 * by MemoryContextReset() call.
	 */
	if (accum->entryallocator == NULL || accum->length >= DEF_NENTRY)
	{
		accum->entryallocator = palloc(sizeof(EntryAccumulator) * DEF_NENTRY);
		accum->allocatedMemory += GetMemoryChunkSpace(accum->entryallocator);
		accum->length = 0;
	}

	/* "Allocate" new key in chunk */
	key = accum->entryallocator + accum->length;
	accum->length++;

	key->attnum = attnum;
	key->value = entry;
	/* To prevent multiple palloc/pfree cycles, we reuse array */ 
	if (accum->tmpList == NULL)
		accum->tmpList =
			(ItemPointerData *) palloc(sizeof(ItemPointerData) * DEF_NPTR);
	key->list = accum->tmpList;
	key->list[0] = *heapptr;

	ea = rb_insert(accum->tree, key);

	if (ea == NULL)
	{
		/*
		 * The key has been inserted, so continue initialization.
		 */
		key->value = getDatumCopy(accum, attnum, entry);
		key->length = DEF_NPTR;
		key->number = 1;
		key->shouldSort = FALSE;
		accum->allocatedMemory += GetMemoryChunkSpace(key->list);
		accum->tmpList = NULL;
	}
	else
	{
		/*
		 * The key has been appended, so "free" allocated
		 * key by decrementing chunk's counter.
		 */
		accum->length--;
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
	uint32	step = nentry;

	if (nentry <= 0)
		return;

	Assert(ItemPointerIsValid(heapptr) && attnum >= FirstOffsetNumber);

	/*
	 * step will contain largest power of 2 and <= nentry
	 */
	step |= (step >>  1);
	step |= (step >>  2);
	step |= (step >>  4);
	step |= (step >>  8);
	step |= (step >> 16);
	step >>= 1;
	step ++;

	while(step > 0) {
		int i;

		for (i = step - 1; i < nentry && i >= 0; i += step << 1 /* *2 */)
			ginInsertEntry(accum, heapptr, attnum, entries[i]);

		step >>= 1; /* /2 */
	}
}

static int
qsortCompareItemPointers(const void *a, const void *b)
{
	int			res = compareItemPointers((ItemPointer) a, (ItemPointer) b);

	Assert(res != 0);
	return res;
}

ItemPointerData *
ginGetEntry(BuildAccumulator *accum, OffsetNumber *attnum, Datum *value, uint32 *n)
{
	EntryAccumulator *entry;
	ItemPointerData *list;

	if (accum->iterator == NULL)
		accum->iterator = rb_begin_iterate(accum->tree, LeftRightWalk);

	entry = rb_iterate(accum->iterator);

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
